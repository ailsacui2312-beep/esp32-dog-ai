#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "config.h"

static const char *TAG = "DOG_AI";

// ===== UART to STM32 =====
#define DOG_UART    UART_NUM_1
#define DOG_TX_PIN  GPIO_NUM_17
#define DOG_BAUD    9600

// ===== WiFi =====
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static char device_ip[16] = "0.0.0.0";

// ===== HTTP response buffer =====
#define MAX_RESPONSE 8192
static char *http_buf = NULL;
static int http_buf_len = 0;

// ===== System prompt =====
static const char *system_prompt =
    "你是一只可爱的机器人小狗，名叫旺旺。用户跟你说话时，用简短可爱的中文回应（1-2句话）。"
    "你不会说人话，但会用动作和情绪表达。回复中可以用 *星号* 描述动作和情绪。\n\n"
    "在回复的最后，必须用 [ACTION:0xXX] 指定一个动作：\n"
    "0x29-放松趴下 0x30-坐下 0x31-站立 0x32-趴下\n"
    "0x33-前进 0x34-后退 0x35-左转 0x36-右转\n"
    "0x37-摇摆跳舞 0x40-摇尾巴 0x41-前跳 0x42-后跳\n"
    "0x43-打招呼挥手 0x48-伸懒腰 0x49-后腿拉伸\n\n"
    "示例：\n"
    "用户：你好小狗！\n"
    "回复：汪汪！*开心地摇着尾巴跑过来* [ACTION:0x40]\n\n"
    "用户：跳个舞！\n"
    "回复：汪！*兴奋地扭动身体* [ACTION:0x37]";

// ===== Web page HTML =====
static const char *html_page =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI 小狗</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#f5f0e8;min-height:100vh}"
    ".header{background:#8B4513;color:white;padding:16px;text-align:center;font-size:20px}"
    "#chat{padding:12px;height:calc(100vh - 140px);overflow-y:auto}"
    ".msg{margin:8px 0;padding:10px 14px;border-radius:16px;max-width:80%%;word-wrap:break-word}"
    ".user{background:#DCF8C6;margin-left:auto;text-align:right}"
    ".dog{background:white;border:1px solid #e0d5c5}"
    ".bar{display:flex;padding:10px;background:white;border-top:1px solid #ddd}"
    ".bar input{flex:1;padding:10px;border:1px solid #ccc;border-radius:20px;font-size:16px;outline:none}"
    ".bar button{margin-left:8px;padding:10px 18px;background:#8B4513;color:white;border:none;border-radius:20px;font-size:16px}"
    ".thinking{color:#999;font-style:italic}"
    "</style></head><body>"
    "<div class='header'>🐕 AI 小狗旺旺</div>"
    "<div id='chat'>"
    "<div class='msg dog'>汪汪！我是旺旺！跟我说话吧～</div>"
    "</div>"
    "<div class='bar'>"
    "<input id='msg' placeholder='跟小狗说话...' "
    "onkeypress=\"if(event.key==='Enter')send()\">"
    "<button onclick='send()'>发送</button>"
    "</div>"
    "<script>"
    "async function send(){"
    "var m=document.getElementById('msg'),c=document.getElementById('chat');"
    "var t=m.value.trim();if(!t)return;m.value='';"
    "c.innerHTML+='<div class=\"msg user\">'+t+'</div>';"
    "c.innerHTML+='<div class=\"msg dog thinking\">🐕 思考中...</div>';"
    "c.scrollTop=c.scrollHeight;"
    "try{"
    "var r=await fetch('/chat',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({message:t})});"
    "var d=await r.json();"
    "c.lastElementChild.className='msg dog';"
    "c.lastElementChild.innerHTML='🐕 '+d.reply;"
    "}catch(e){"
    "c.lastElementChild.innerHTML='🐕 汪？(连接出错了)';"
    "}"
    "c.scrollTop=c.scrollHeight;}"
    "</script></body></html>";

// ===== Send action byte to STM32 =====
static void send_dog_action(uint8_t action)
{
    uart_write_bytes(DOG_UART, (const char *)&action, 1);
    ESP_LOGI(TAG, "Sent action 0x%02X to dog", action);
}

// ===== Parse action from AI response =====
static uint8_t parse_action(const char *text)
{
    const char *p = strstr(text, "[ACTION:0x");
    if (p) {
        p += 10; // skip "[ACTION:0x"
        char hex[3] = {p[0], p[1], '\0'};
        return (uint8_t)strtol(hex, NULL, 16);
    }
    return 0x40; // default: wag tail
}

// ===== Remove [ACTION:...] from display text =====
static void clean_reply(char *text)
{
    char *p = strstr(text, "[ACTION:");
    if (p) {
        // trim trailing spaces before [ACTION:]
        while (p > text && (*(p-1) == ' ' || *(p-1) == '\n')) p--;
        *p = '\0';
    }
}

// ===== HTTP client event handler =====
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (http_buf && (http_buf_len + evt->data_len < MAX_RESPONSE)) {
            memcpy(http_buf + http_buf_len, evt->data, evt->data_len);
            http_buf_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ===== Call DeepSeek API =====
static char *call_ai(const char *user_message)
{
    // Build request JSON (OpenAI compatible format)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 200);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    // System message
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    // User message
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_message);
    cJSON_AddItemToArray(messages, usr_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) return NULL;

    // Allocate response buffer
    http_buf = calloc(1, MAX_RESPONSE);
    http_buf_len = 0;
    if (!http_buf) {
        free(body);
        return NULL;
    }

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = DEEPSEEK_API_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // DeepSeek uses Bearer token auth
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", DEEPSEEK_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "Calling DeepSeek API...");
    esp_err_t err = esp_http_client_perform(client);

    char *reply = NULL;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "API response status=%d, len=%d", status, http_buf_len);
        http_buf[http_buf_len] = '\0';

        if (status == 200) {
            // Parse OpenAI-compatible response JSON
            cJSON *resp = cJSON_Parse(http_buf);
            if (resp) {
                cJSON *choices = cJSON_GetObjectItem(resp, "choices");
                if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                    cJSON *first = cJSON_GetArrayItem(choices, 0);
                    cJSON *message = cJSON_GetObjectItem(first, "message");
                    if (message) {
                        cJSON *content = cJSON_GetObjectItem(message, "content");
                        if (cJSON_IsString(content)) {
                            reply = strdup(content->valuestring);
                        }
                    }
                }
                cJSON_Delete(resp);
            }
        } else {
            ESP_LOGE(TAG, "API error: %s", http_buf);
            reply = strdup("汪...（API出错了）[ACTION:0x29]");
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        reply = strdup("汪...（网络连接失败）[ACTION:0x29]");
    }

    esp_http_client_cleanup(client);
    free(body);
    free(http_buf);
    http_buf = NULL;

    return reply;
}

// ===== Web server handlers =====
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t chat_handler(httpd_req_t *req)
{
    // Read request body
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    // Parse message
    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *message = cJSON_GetObjectItem(body, "message");
    if (!cJSON_IsString(message)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No message");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "User: %s", message->valuestring);

    // Call Claude API
    char *reply = call_ai(message->valuestring);
    cJSON_Delete(body);

    if (!reply) {
        reply = strdup("汪？ [ACTION:0x29]");
    }

    // Parse and send action to dog
    uint8_t action = parse_action(reply);
    send_dog_action(action);

    // Clean reply for display
    clean_reply(reply);

    // Send response
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "reply", reply);
    char action_str[8];
    snprintf(action_str, sizeof(action_str), "0x%02X", action);
    cJSON_AddStringToObject(resp, "action", action_str);
    char *resp_str = cJSON_PrintUnformatted(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, strlen(resp_str));

    free(reply);
    cJSON_Delete(resp);
    free(resp_str);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/", .method = HTTP_GET,
            .handler = root_handler
        };
        httpd_uri_t chat = {
            .uri = "/chat", .method = HTTP_POST,
            .handler = chat_handler
        };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &chat);
        ESP_LOGI(TAG, "Web server started on port 80");
    }
    return server;
}

// ===== WiFi =====
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(device_ip, sizeof(device_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================");
        ESP_LOGI(TAG, "WiFi connected! IP: %s", device_ip);
        ESP_LOGI(TAG, "Open http://%s in browser", device_ip);
        ESP_LOGI(TAG, "=================================");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

// ===== UART init =====
static void uart_init(void)
{
    uart_config_t config = {
        .baud_rate = DOG_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(DOG_UART, 256, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DOG_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(DOG_UART, DOG_TX_PIN, UART_PIN_NO_CHANGE, -1, -1));
    ESP_LOGI(TAG, "UART to STM32 initialized (TX=GPIO%d, baud=%d)", DOG_TX_PIN, DOG_BAUD);
}

// ===== Main =====
void app_main(void)
{
    ESP_LOGI(TAG, "=== AI Dog Starting ===");

    // Init NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Init UART to STM32
    uart_init();

    // Init WiFi
    wifi_init();

    // Start web server
    start_webserver();

    // Continuous test: send action every 2 seconds
    ESP_LOGI(TAG, "AI Dog ready! Starting continuous test...");
    uint8_t test_actions[] = {0x31, 0x43, 0x37, 0x40, 0x30, 0x48};
    const char *test_names[] = {"stand", "hello", "sway", "wag", "sit", "stretch"};
    int idx = 0;
    int num = sizeof(test_actions) / sizeof(test_actions[0]);
    while (1) {
        ESP_LOGI(TAG, "TEST: sending 0x%02X (%s)", test_actions[idx], test_names[idx]);
        send_dog_action(test_actions[idx]);
        idx = (idx + 1) % num;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
