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
#include "esp_random.h"

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

// ===== Action queue (async execution) =====
#define MAX_ACTIONS 3

typedef struct {
    uint8_t actions[MAX_ACTIONS];
    int count;
} action_request_t;

static QueueHandle_t action_queue = NULL;
static volatile bool tail_wag_on = false;  // track 0x40 toggle state

// ===== Async AI chat =====
static SemaphoreHandle_t ai_mutex = NULL;  // protects call_ai (one at a time)

typedef enum {
    CHAT_IDLE,
    CHAT_PROCESSING,
    CHAT_DONE
} chat_state_t;

static volatile chat_state_t chat_state = CHAT_IDLE;
static char chat_request[256] = {0};     // user message
static char chat_reply[1024] = {0};      // AI reply (cleaned)
static char chat_action_str[32] = {0};   // action codes string
static uint8_t chat_actions[MAX_ACTIONS];
static int chat_action_count = 0;

// ===== Debug log buffer =====
#define LOG_BUF_SIZE 2048
static char debug_log[LOG_BUF_SIZE] = {0};
static int debug_log_len = 0;

static void log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0) return;

    // Append with wrap
    if (debug_log_len + n + 1 >= LOG_BUF_SIZE) {
        debug_log_len = 0;  // simple wrap
    }
    memcpy(debug_log + debug_log_len, line, n);
    debug_log_len += n;
    debug_log[debug_log_len++] = '\n';
    debug_log[debug_log_len] = '\0';
}

// ===== System prompt =====
static const char *system_prompt =
    "你是一只可爱的机器人小狗，名叫旺旺。用户跟你说话时，用简短可爱的中文回应（1-2句话）。"
    "你不会说人话，但会用动作和情绪表达。回复中可以用 *星号* 描述动作和情绪。\n\n"
    "在回复的最后，用 [ACTION:0xXX] 指定动作序列（1-3个动作，按顺序执行，最后会自动睡觉）：\n"
    "0x30-蹲下 0x31-站立 0x32-趴下 0x33-前进 0x34-后退\n"
    "0x35-左转 0x36-右转 0x37-摇摆舞\n"
    "0x40-摇尾巴(开关) 0x41-前跳 0x42-后跳\n"
    "0x43-打招呼挥手 0x48-伸懒腰 0x49-拉伸腿\n"
    "注意：0x40摇尾巴是开关，发一次开始摇，动作结束会自动关闭。\n\n"
    "示例：\n"
    "用户：你好小狗！\n"
    "回复：汪汪！*开心地站起来摇尾巴打招呼* [ACTION:0x31][ACTION:0x40][ACTION:0x43]\n\n"
    "用户：过来！\n"
    "回复：汪！*兴奋地跑过来* [ACTION:0x33][ACTION:0x40]\n\n"
    "用户：你累了吧\n"
    "回复：*打了个哈欠伸伸懒腰* [ACTION:0x48]";

// ===== Web page HTML (async polling version) =====
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
    "var sending=false;"
    "async function send(){"
    "if(sending)return;"
    "var m=document.getElementById('msg'),c=document.getElementById('chat');"
    "var t=m.value.trim();if(!t)return;m.value='';"
    "c.innerHTML+='<div class=\"msg user\">'+t+'</div>';"
    "c.innerHTML+='<div class=\"msg dog thinking\">🐕 思考中...</div>';"
    "c.scrollTop=c.scrollHeight;"
    "sending=true;"
    "try{"
    // Step 1: Send message (returns immediately)
    "var r=await fetch('/chat',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({message:t})});"
    "var d=await r.json();"
    "if(d.status==='processing'){"
    // Step 2: Poll for result every 1.5 seconds
    "for(var i=0;i<30;i++){"
    "await new Promise(r=>setTimeout(r,1500));"
    "var p=await fetch('/reply');var pr=await p.json();"
    "if(pr.status==='done'){"
    "c.lastElementChild.className='msg dog';"
    "c.lastElementChild.innerHTML='🐕 '+pr.reply;"
    "c.scrollTop=c.scrollHeight;sending=false;return;"
    "}"
    "}"
    "c.lastElementChild.innerHTML='🐕 *歪头* 汪？(超时了)';"
    "}else if(d.reply){"
    "c.lastElementChild.className='msg dog';"
    "c.lastElementChild.innerHTML='🐕 '+d.reply;"
    "}"
    "}catch(e){"
    "c.lastElementChild.className='msg dog';"
    "c.lastElementChild.innerHTML='🐕 *歪头* 网络错误: '+e.message;"
    "}"
    "c.scrollTop=c.scrollHeight;sending=false;}"
    "</script></body></html>";

// ===== Send action byte to STM32 =====
static void send_dog_action(uint8_t action)
{
    uart_write_bytes(DOG_UART, (const char *)&action, 1);
    ESP_LOGI(TAG, "Sent action 0x%02X to dog", action);
}

// ===== Stop dog: turn off tail wag if on, then sleep =====
static void stop_dog(void)
{
    if (tail_wag_on) {
        send_dog_action(0x40);  // toggle OFF tail wag
        vTaskDelay(pdMS_TO_TICKS(300));
        tail_wag_on = false;
        ESP_LOGI(TAG, "Stop: wag OFF");
    }
    send_dog_action(0x29);  // 睡觉 = stop all movement!
    ESP_LOGI(TAG, "Stop: sleep (0x29)");
}

// ===== Action executor task (async, serial protected) =====
static void action_executor_task(void *arg)
{
    action_request_t req;
    while (1) {
        if (xQueueReceive(action_queue, &req, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Executing %d actions", req.count);
            log_debug("Exec %d actions", req.count);
            for (int i = 0; i < req.count; i++) {
                uint8_t act = req.actions[i];
                if (act == 0x40) {
                    tail_wag_on = !tail_wag_on;  // track toggle
                    ESP_LOGI(TAG, "Tail wag toggled %s", tail_wag_on ? "ON" : "OFF");
                }
                send_dog_action(act);
                vTaskDelay(pdMS_TO_TICKS(2500)); // let each action play
            }
            stop_dog();
            log_debug("Actions done, stopped");
        }
    }
}

// ===== Queue actions for async execution =====
static void queue_actions(uint8_t *actions, int count)
{
    action_request_t req;
    req.count = count > MAX_ACTIONS ? MAX_ACTIONS : count;
    memcpy(req.actions, actions, req.count);
    // Clear queue first (discard old pending actions)
    action_request_t discard;
    while (xQueueReceive(action_queue, &discard, 0) == pdTRUE) {}
    xQueueSend(action_queue, &req, 0);
}

// ===== Parse action(s) from AI response =====
static int parse_actions(const char *text, uint8_t *actions)
{
    int count = 0;
    const char *p = text;
    while (count < MAX_ACTIONS && (p = strstr(p, "[ACTION:0x")) != NULL) {
        p += 10; // skip "[ACTION:0x"
        char hex[3] = {p[0], p[1], '\0'};
        actions[count++] = (uint8_t)strtol(hex, NULL, 16);
    }
    if (count == 0) {
        actions[0] = 0x40; // default: wag tail
        count = 1;
    }
    return count;
}

// ===== Remove [ACTION:...] from display text =====
static void clean_reply(char *text)
{
    char *p = strstr(text, "[ACTION:");
    if (p) {
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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", 200);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_message);
    cJSON_AddItemToArray(messages, usr_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) return NULL;

    http_buf = calloc(1, MAX_RESPONSE);
    http_buf_len = 0;
    if (!http_buf) {
        free(body);
        return NULL;
    }

    esp_http_client_config_t config = {
        .url = DEEPSEEK_API_URL,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,  // reduced from 30s
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", DEEPSEEK_API_KEY);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    log_debug("Calling DeepSeek API...");
    ESP_LOGI(TAG, "Calling DeepSeek API...");
    esp_err_t err = esp_http_client_perform(client);

    char *reply = NULL;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "API status=%d, len=%d", status, http_buf_len);
        log_debug("API status=%d len=%d", status, http_buf_len);
        http_buf[http_buf_len] = '\0';

        if (status == 200) {
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
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "*歪头困惑* API错误(%d) [ACTION:0x30]", status);
            reply = strdup(errbuf);
            log_debug("API error %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP failed: %s", esp_err_to_name(err));
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "*歪头困惑* 网络错误 [ACTION:0x30]");
        reply = strdup(errbuf);
        log_debug("HTTP failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(body);
    free(http_buf);
    http_buf = NULL;

    return reply;
}

// ===== AI worker task (runs API call off httpd thread) =====
static void ai_worker_task(void *arg)
{
    while (1) {
        // Wait until we have a chat request
        while (chat_state != CHAT_PROCESSING) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        log_debug("AI worker: processing '%s'", chat_request);

        // Take mutex to serialize API calls
        xSemaphoreTake(ai_mutex, portMAX_DELAY);
        char *reply = call_ai(chat_request);
        xSemaphoreGive(ai_mutex);

        if (!reply) {
            reply = strdup("汪？ *歪头* [ACTION:0x30]");
        }

        // Parse actions
        chat_action_count = parse_actions(reply, chat_actions);

        // Build action string
        int offset = 0;
        for (int i = 0; i < chat_action_count; i++) {
            offset += snprintf(chat_action_str + offset,
                              sizeof(chat_action_str) - offset,
                              "%s0x%02X", i > 0 ? "," : "", chat_actions[i]);
        }

        // Clean and store reply
        clean_reply(reply);
        strncpy(chat_reply, reply, sizeof(chat_reply) - 1);
        chat_reply[sizeof(chat_reply) - 1] = '\0';
        free(reply);

        // Queue actions
        queue_actions(chat_actions, chat_action_count);

        log_debug("AI worker: done, reply='%.50s'", chat_reply);

        // Signal done
        chat_state = CHAT_DONE;
    }
}

// ===== Web server handlers =====
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

// POST /chat — accepts message, returns immediately
static esp_err_t chat_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

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
    log_debug("Chat: user='%s'", message->valuestring);

    // If already processing, reject
    if (chat_state == CHAT_PROCESSING) {
        cJSON_Delete(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"busy\",\"reply\":\"*忙着呢* 汪！\"}");
        return ESP_OK;
    }

    // Store request and signal worker
    strncpy(chat_request, message->valuestring, sizeof(chat_request) - 1);
    chat_request[sizeof(chat_request) - 1] = '\0';
    cJSON_Delete(body);

    chat_state = CHAT_PROCESSING;

    // Return immediately!
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"processing\"}");
    return ESP_OK;
}

// GET /reply — poll for AI response
static esp_err_t reply_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (chat_state == CHAT_DONE) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "done");
        cJSON_AddStringToObject(resp, "reply", chat_reply);
        cJSON_AddStringToObject(resp, "action", chat_action_str);
        char *resp_str = cJSON_PrintUnformatted(resp);
        httpd_resp_send(req, resp_str, strlen(resp_str));
        free(resp_str);
        cJSON_Delete(resp);
        chat_state = CHAT_IDLE;
    } else if (chat_state == CHAT_PROCESSING) {
        httpd_resp_sendstr(req, "{\"status\":\"processing\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"idle\"}");
    }
    return ESP_OK;
}

// GET /test — diagnostic
static esp_err_t test_handler(httpd_req_t *req)
{
    char result[512] = {0};
    int len = 0;

    len += snprintf(result + len, sizeof(result) - len, "Testing DeepSeek API...\n");

    xSemaphoreTake(ai_mutex, portMAX_DELAY);
    char *reply = call_ai("test");
    xSemaphoreGive(ai_mutex);

    if (reply) {
        len += snprintf(result + len, sizeof(result) - len, "OK: %s\n", reply);
        free(reply);
    } else {
        len += snprintf(result + len, sizeof(result) - len, "FAILED: NULL\n");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, result, len);
    return ESP_OK;
}

// GET /log — view debug log from phone
static esp_err_t log_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    if (debug_log_len > 0) {
        httpd_resp_send(req, debug_log, debug_log_len);
    } else {
        httpd_resp_sendstr(req, "(no logs yet)\n");
    }
    return ESP_OK;
}

// GET /cmd?b=XX — send a raw byte to STM32 (for finding stop command)
static esp_err_t cmd_handler(httpd_req_t *req)
{
    char qstr[32] = {0};
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(qstr, "b", val, sizeof(val)) == ESP_OK) {
            uint8_t byte = (uint8_t)strtol(val, NULL, 16);
            send_dog_action(byte);
            log_debug("CMD: sent 0x%02X", byte);
            char resp[64];
            snprintf(resp, sizeof(resp), "Sent 0x%02X\n", byte);
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, resp, strlen(resp));
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use /cmd?b=30");
    return ESP_FAIL;
}

// GET /remote — test console to try different bytes
static const char *remote_page =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>遥控器</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#f5f0e8;padding:12px}"
    "h2{text-align:center;color:#8B4513;margin:10px 0}"
    ".sec{margin:12px 0}"
    ".sec h3{color:#666;font-size:14px;margin-bottom:6px}"
    "button{padding:10px 14px;margin:3px;border:1px solid #ccc;border-radius:8px;"
    "background:white;font-size:14px;cursor:pointer;min-width:70px}"
    "button:active{background:#8B4513;color:white}"
    ".stop{background:#ff4444;color:white;border:none;font-size:16px;padding:12px 20px}"
    "#log{margin-top:12px;padding:8px;background:#333;color:#0f0;font-size:12px;"
    "font-family:monospace;border-radius:8px;min-height:60px;white-space:pre-wrap}"
    ".custom{display:flex;gap:6px;align-items:center;margin-top:8px}"
    ".custom input{padding:8px;border:1px solid #ccc;border-radius:8px;width:80px;font-size:16px}"
    "</style></head><body>"
    "<h2>🎮 小狗遥控器</h2>"

    "<div class='sec'><h3>🔍 找停止命令 (点击试哪个能让小狗停)</h3>"
    "<button onclick='s(\"00\")'>0x00</button>"
    "<button onclick='s(\"29\")'>0x29</button>"
    "<button onclick='s(\"38\")'>0x38</button>"
    "<button onclick='s(\"39\")'>0x39</button>"
    "<button onclick='s(\"3A\")'>0x3A</button>"
    "<button onclick='s(\"3B\")'>0x3B</button>"
    "<button onclick='s(\"3C\")'>0x3C</button>"
    "<button onclick='s(\"3D\")'>0x3D</button>"
    "<button onclick='s(\"3E\")'>0x3E</button>"
    "<button onclick='s(\"3F\")'>0x3F</button>"
    "<button onclick='s(\"FF\")'>0xFF</button>"
    "</div>"

    "<div class='sec'><h3>🐕 已知动作</h3>"
    "<button onclick='s(\"30\")'>坐下</button>"
    "<button onclick='s(\"31\")'>站立</button>"
    "<button onclick='s(\"32\")'>趴下</button>"
    "<button onclick='s(\"33\")'>前进</button>"
    "<button onclick='s(\"34\")'>后退</button>"
    "<button onclick='s(\"35\")'>左转</button>"
    "<button onclick='s(\"36\")'>右转</button>"
    "<button onclick='s(\"37\")'>跳舞</button>"
    "<button onclick='s(\"40\")'>摇尾巴</button>"
    "<button onclick='s(\"41\")'>前跳</button>"
    "<button onclick='s(\"42\")'>后跳</button>"
    "<button onclick='s(\"43\")'>招手</button>"
    "<button onclick='s(\"48\")'>伸懒腰</button>"
    "<button onclick='s(\"49\")'>后腿拉伸</button>"
    "</div>"

    "<div class='sec'><h3>🔢 自定义字节</h3>"
    "<div class='custom'>"
    "<span>0x</span>"
    "<input id='hex' type='text' maxlength='2' placeholder='3E'>"
    "<button onclick='s(document.getElementById(\"hex\").value)'>发送</button>"
    "</div></div>"

    "<div id='log'>等待操作...</div>"

    "<script>"
    "async function s(h){"
    "var l=document.getElementById('log');"
    "l.textContent+='\\n发送 0x'+h.toUpperCase()+'...';"
    "try{var r=await fetch('/cmd?b='+h);"
    "var t=await r.text();"
    "l.textContent+=' '+t;"
    "}catch(e){l.textContent+=' 错误:'+e.message+'\\n';}"
    "l.scrollTop=l.scrollHeight;}"
    "</script></body></html>";

static esp_err_t remote_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, remote_page, strlen(remote_page));
    return ESP_OK;
}

// ===== External action API =====
static esp_err_t action_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *actions_arr = cJSON_GetObjectItem(body, "actions");
    if (!cJSON_IsArray(actions_arr)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need actions array");
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(actions_arr);
    if (count > MAX_ACTIONS) count = MAX_ACTIONS;

    uint8_t action_bytes[MAX_ACTIONS];
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(actions_arr, i);
        if (cJSON_IsNumber(item)) {
            action_bytes[i] = (uint8_t)item->valueint;
        }
    }
    queue_actions(action_bytes, count);

    cJSON_Delete(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_uri_t chat = { .uri = "/chat", .method = HTTP_POST, .handler = chat_handler };
        httpd_uri_t reply = { .uri = "/reply", .method = HTTP_GET, .handler = reply_handler };
        httpd_uri_t action = { .uri = "/action", .method = HTTP_POST, .handler = action_handler };
        httpd_uri_t test = { .uri = "/test", .method = HTTP_GET, .handler = test_handler };
        httpd_uri_t logep = { .uri = "/log", .method = HTTP_GET, .handler = log_handler };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &chat);
        httpd_register_uri_handler(server, &reply);
        httpd_register_uri_handler(server, &action);
        httpd_register_uri_handler(server, &test);
        httpd_register_uri_handler(server, &logep);
        httpd_uri_t cmd = { .uri = "/cmd", .method = HTTP_GET, .handler = cmd_handler };
        httpd_uri_t remote = { .uri = "/remote", .method = HTTP_GET, .handler = remote_handler };
        httpd_register_uri_handler(server, &cmd);
        httpd_register_uri_handler(server, &remote);
        ESP_LOGI(TAG, "Web server started on port 80");
    }
    return server;
}

// ===== Idle behavior task =====
static void idle_behavior_task(void *arg)
{
    typedef struct {
        uint8_t actions[3];
        int count;
        const char *name;
    } behavior_t;

    behavior_t behaviors[] = {
        {{0x48, 0x31, 0x40}, 3, "stretch and wag"},
        {{0x40},             1, "wag tail"},
        {{0x35, 0x36},       2, "look around"},
        {{0x48},             1, "stretch"},
        {{0x33, 0x34},       2, "pace around"},
        {{0x49, 0x48},       2, "leg stretch"},
        {{0x31, 0x40},       2, "stand and wag"},
        {{0x49},             1, "rear stretch"},
    };
    int num_behaviors = sizeof(behaviors) / sizeof(behaviors[0]);
    int first_run = 1;

    while (1) {
        int delay_sec = first_run ? 60 + (esp_random() % 60) : 180 + (esp_random() % 120);
        first_run = 0;
        ESP_LOGI(TAG, "Next idle in %d sec", delay_sec);
        log_debug("Idle: next in %ds", delay_sec);
        vTaskDelay(pdMS_TO_TICKS(delay_sec * 1000));

        int pick = esp_random() % num_behaviors;
        behavior_t *b = &behaviors[pick];
        ESP_LOGI(TAG, "Idle: %s", b->name);
        log_debug("Idle: %s", b->name);

        queue_actions(b->actions, b->count);
    }
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
        ESP_LOGI(TAG, "WiFi connected! IP: %s", device_ip);
        log_debug("WiFi OK, IP=%s", device_ip);
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
    ESP_LOGI(TAG, "UART init (TX=GPIO%d, baud=%d)", DOG_TX_PIN, DOG_BAUD);
}

// ===== Main =====
void app_main(void)
{
    ESP_LOGI(TAG, "=== AI Dog Starting ===");
    log_debug("=== AI Dog Starting ===");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uart_init();
    wifi_init();

    // Init async AI system
    ai_mutex = xSemaphoreCreateMutex();
    action_queue = xQueueCreate(3, sizeof(action_request_t));
    xTaskCreate(action_executor_task, "action_exec", 4096, NULL, 6, NULL);
    xTaskCreate(ai_worker_task, "ai_worker", 8192, NULL, 5, NULL);

    start_webserver();

    // Sit on startup
    send_dog_action(0x30);

    // Idle behaviors disabled for now (testing stop behavior)
    // xTaskCreate(idle_behavior_task, "idle_dog", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "AI Dog ready!");
    log_debug("Ready! Endpoints: / /chat /reply /test /log /action");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
