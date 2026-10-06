#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "agent_led.h"
#define WIFI_OK BIT0
#define FAILED BIT1
#define BODY_CAPACITY (64 * 1024)
static const char *TAG = "deepseek_v2";
static EventGroupHandle_t events;
static unsigned retries;
#define HISTORY_TURNS 4
#define HISTORY_BYTES (12 * 1024)
typedef struct { char *user; char *assistant; size_t bytes; } history_turn_t;
static history_turn_t history[HISTORY_TURNS];
static size_t history_count, history_bytes;
static void history_drop_oldest(void) {
    if (!history_count) return;
    history_bytes -= history[0].bytes;
    free(history[0].user); free(history[0].assistant);
    --history_count;
    memmove(history, history + 1, history_count * sizeof(history[0]));
    memset(&history[history_count], 0, sizeof(history[0]));
}
static void history_clear(void) {
    while (history_count) history_drop_oldest();
}
/* Allocate both strings before eviction: failures leave existing history intact. */
static bool history_commit(const char *user, const char *assistant) {
    size_t u = strlen(user) + 1, a = strlen(assistant) + 1;
    if (u > HISTORY_BYTES || a > HISTORY_BYTES - u) return false;
    char *ucopy = malloc(u), *acopy = malloc(a);
    if (!ucopy || !acopy) { free(ucopy); free(acopy); return false; }
    memcpy(ucopy, user, u); memcpy(acopy, assistant, a);
    while (history_count >= HISTORY_TURNS || history_bytes + u + a > HISTORY_BYTES)
        history_drop_oldest();
    history[history_count++] = (history_turn_t){ucopy, acopy, u + a};
    history_bytes += u + a; return true;
}

static void read_line(const char *label, char *out, size_t cap, bool allow_empty, bool secret) {
    static bool skip_lf;
    for (;;) {
        printf("\n%s\n", label); fflush(stdout);
        size_t n = 0; bool overflow = false;
        unsigned escape_state = 0;
        for (;;) {
            int c = getchar();
            if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            /* Consume ANSI CSI/SS3 sequences, including bracketed-paste wrappers.
             * Ignoring only ESC would incorrectly append '[200~' to the key. */
            if (c == 27) { escape_state = 1; continue; }
            if (escape_state == 1) {
                escape_state = (c == '[' || c == 'O') ? 2 : 0;
                continue;
            }
            if (escape_state == 2) {
                if (c >= 0x40 && c <= 0x7e) escape_state = 0;
                continue;
            }
            if (skip_lf && c == '\n') { skip_lf = false; continue; }
            skip_lf = false;
            if (c == '\r' || c == '\n') { skip_lf = c == '\r'; putchar('\n'); fflush(stdout); break; }
            if (c == 8 || c == 127) {
                if (n) { --n; printf("\b \b"); fflush(stdout); }
                continue;
            }
            if (c < 32) { putchar('\a'); fflush(stdout); continue; }
            if (n + 1 < cap) {
                out[n++] = (char)c;
                putchar(secret ? '*' : c); fflush(stdout);
            } else { overflow = true; putchar('\a'); fflush(stdout); }
        }
        out[n] = 0;
        if (!overflow && (n || allow_empty)) { printf("Received %u bytes.\n", (unsigned)n); return; }
        puts("Empty or too long; enter again.");
    }
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u; attempt=%u/6",
                 disconnected ? (unsigned)disconnected->reason : 0u, retries + 1);
        xEventGroupClearBits(events, WIFI_OK);
        if (retries++ < 5) esp_wifi_connect();
        else xEventGroupSetBits(events, FAILED);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        retries = 0; xEventGroupClearBits(events, FAILED); xEventGroupSetBits(events, WIFI_OK);
    }
}

typedef struct { char *data; size_t used; bool overflow; int64_t started; } response_t;
static esp_err_t http_event(esp_http_client_event_t *event) {
    response_t *r = event->user_data;
    if (esp_timer_get_time() - r->started > 90000000) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t n = (size_t)event->data_len;
        if (n >= BODY_CAPACITY - r->used) { r->overflow = true; return ESP_FAIL; }
        memcpy(r->data + r->used, event->data, n);
        r->used += n; r->data[r->used] = 0;
    }
    return ESP_OK;
}
static void trim_key(char *key) {
    size_t n = strlen(key);
    while (n && isspace((unsigned char)key[n - 1])) key[--n] = 0;
    size_t start = 0; while (key[start] && isspace((unsigned char)key[start])) ++start;
    if (start) memmove(key, key + start, strlen(key + start) + 1);
}
static void show_error(int status) {
    switch (status) {
        case 400: ESP_LOGE(TAG, "Request rejected: check model and JSON parameters"); break;
        case 401: ESP_LOGE(TAG, "Authentication rejected: use a DeepSeek API key"); break;
        case 402: ESP_LOGE(TAG, "Check DeepSeek account balance"); break;
        case 429: ESP_LOGE(TAG, "Rate limit; retry later manually"); break;
        default: ESP_LOGE(TAG, "Request failed; HTTP status=%d", status); break;
    }
}
static bool append_message(cJSON *messages, const char *role, const char *content) {
    cJSON *message = cJSON_CreateObject();
    if (!message) return false;
    if (!cJSON_AddStringToObject(message, "role", role) ||
        !cJSON_AddStringToObject(message, "content", content) ||
        !cJSON_AddItemToArray(messages, message)) {
        cJSON_Delete(message); return false;
    }
    return true;
}
/* Transport returns the complete assistant message, including tool_calls. */
static cJSON *post_chat(const char *key, const cJSON *request) {
    char *body = cJSON_PrintUnformatted(request);
    if (!body) return NULL;
    response_t response = {.data = calloc(1, BODY_CAPACITY), .started = esp_timer_get_time()};
    if (!response.data) { free(body); return NULL; }
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event, .user_data = &response,
        .timeout_ms = 60000, .buffer_size = 2048, .buffer_size_tx = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(body); free(response.data); return NULL; }
    char authorization[sizeof("Bearer ") + 256];
    snprintf(authorization, sizeof(authorization), "Bearer %s", key);
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, sizeof(authorization));
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, strlen(body));
    ESP_LOGI(TAG, "HTTPS POST; model=%s; previous turns=%u", CONFIG_DEEPSEEK_MODEL, (unsigned)history_count);
    if (err == ESP_OK) err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d; response bytes=%u", status, (unsigned)response.used);
    cJSON *answer = NULL;
    if (response.overflow) ESP_LOGE(TAG, "Response exceeded 64 KiB limit");
    else if (err != ESP_OK) ESP_LOGE(TAG, "HTTPS failed: %s", esp_err_to_name(err));
    else if (status != 200) show_error(status);
    else {
        cJSON *root = cJSON_Parse(response.data);
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *reply = cJSON_GetObjectItemCaseSensitive(first, "message");
        cJSON *finish = cJSON_GetObjectItemCaseSensitive(first, "finish_reason");
        if (cJSON_IsString(finish) &&
            (!strcmp(finish->valuestring, "stop") || !strcmp(finish->valuestring, "tool_calls")) &&
            cJSON_IsObject(reply)) answer = cJSON_Duplicate(reply, true);
        else ESP_LOGE(TAG, "Incomplete or invalid response; no tool execution");
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "Request time=%lld ms; saved turns=%u; history bytes=%u",
             (long long)((esp_timer_get_time() - response.started) / 1000),
             (unsigned)history_count, (unsigned)history_bytes);
    esp_http_client_cleanup(client); free(body); free(response.data);
    return answer;
}

static const char *tool_schema =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_led\","
    "\"description\":\"Set onboard RGB LED. One static color per user turn; no blinking.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"color\":{\"type\":\"string\",\"enum\":[\"off\",\"red\",\"green\",\"blue\",\"yellow\",\"cyan\",\"purple\",\"white\"]},"
    "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"color\",\"brightness\"],\"additionalProperties\":false}}}]";

static bool ask_deepseek(const char *key, const char *prompt) {
    bool ok = false, acted = false;
    int64_t started = esp_timer_get_time();
    cJSON *request = cJSON_CreateObject(), *reply = NULL;
    if (!request) return false;
    cJSON *thinking = cJSON_AddObjectToObject(request, "thinking");
    cJSON *messages = cJSON_AddArrayToObject(request, "messages");
    cJSON *tools = cJSON_Parse(tool_schema);
    if (!tools) goto cleanup;
    if (!cJSON_AddItemToObject(request, "tools", tools)) { cJSON_Delete(tools); goto cleanup; }
    if (!thinking || !messages ||
        !cJSON_AddStringToObject(thinking, "type", "disabled") ||
        !cJSON_AddStringToObject(request, "model", CONFIG_DEEPSEEK_MODEL) ||
        !cJSON_AddBoolToObject(request, "stream", false) ||
        !cJSON_AddNumberToObject(request, "max_tokens", 512)) goto cleanup;
    char state[128], system[768];
    agent_led_state(state, sizeof(state));
    snprintf(system, sizeof(system),
        "You are an ESP32 assistant. Reply briefly in the user's language. "
        "For an explicit request to change this board's LED, call set_led exactly once. "
        "Never claim a change without a successful tool result. Brightness defaults to 50. "
        "Only static colors are supported. No timers, blinking or other GPIO control. "
        "For multiple colors or sequences ask the user to choose one. "
        "Current firmware state: %s. This state supersedes earlier chat. "
        "No physical light sensor is present.", state);
    if (!append_message(messages, "system", system)) goto cleanup;
    for (size_t i = 0; i < history_count; ++i)
        if (!append_message(messages, "user", history[i].user) ||
            !append_message(messages, "assistant", history[i].assistant)) goto cleanup;
    if (!append_message(messages, "user", prompt)) goto cleanup;
    reply = post_chat(key, request);
    if (!reply) goto cleanup;
    cJSON *calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
    if (calls && !cJSON_IsNull(calls) && !cJSON_IsArray(calls)) goto cleanup;
    int count = cJSON_GetArraySize(calls);
    if (count > 1) {
        puts("Tool request rejected: only one LED action per turn. No action executed.");
        goto cleanup;
    }
    if (count == 1) {
        cJSON *call = cJSON_GetArrayItem(calls, 0);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        cJSON *type = cJSON_GetObjectItemCaseSensitive(call, "type");
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
        if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) > 256 ||
            !cJSON_IsString(type) || strcmp(type->valuestring, "function") ||
            !cJSON_IsString(name) || !cJSON_IsString(args)) goto cleanup;
        /* Attach reply and allocate result envelope before touching hardware. */
        if (!cJSON_AddItemToArray(messages, reply)) goto cleanup;
        reply = NULL; /* request owns the message and its id/name/args now */
        cJSON *result_message = cJSON_CreateObject();
        if (!result_message) goto cleanup;
        if (!cJSON_AddStringToObject(result_message, "role", "tool") ||
            !cJSON_AddStringToObject(result_message, "tool_call_id", id->valuestring)) {
            cJSON_Delete(result_message); goto cleanup;
        }
        if (!cJSON_AddItemToArray(messages, result_message)) {
            cJSON_Delete(result_message); goto cleanup;
        }
        char result[192];
        acted = agent_led_execute(name->valuestring, args->valuestring, result, sizeof(result));
        printf("TOOL set_led: %s\n", result);
        if (!cJSON_AddStringToObject(result_message, "content", result) ||
            !cJSON_AddStringToObject(request, "tool_choice", "none")) goto cleanup;
        reply = post_chat(key, request); /* At most two HTTP requests per turn. */
        if (!reply) goto cleanup;
        calls = cJSON_GetObjectItemCaseSensitive(reply, "tool_calls");
        if (calls && !cJSON_IsNull(calls) &&
            (!cJSON_IsArray(calls) || cJSON_GetArraySize(calls) != 0)) {
            puts("Unexpected extra tool request ignored; action limit reached."); goto cleanup;
        }
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(reply, "content");
    if (!cJSON_IsString(content) || !content->valuestring[0]) goto cleanup;
    printf("\nDeepSeek: %s\n", content->valuestring);
    ok = true;
    if (!history_commit(prompt, content->valuestring))
        ESP_LOGW(TAG, "Reply displayed but not saved (history capacity/allocation)");
cleanup:
    if (!ok && acted)
        puts("LED command succeeded, but final reply failed. LED change was NOT rolled back; no automatic resend.");
    cJSON_Delete(reply); cJSON_Delete(request);
    ESP_LOGI(TAG, "Turn time=%lld ms; saved turns=%u; history bytes=%u",
             (long long)((esp_timer_get_time()-started)/1000),
             (unsigned)history_count, (unsigned)history_bytes);
    return ok;
}

void app_main(void) {
    puts("\nFirmware: DEEPSEEK-V2-LED-20261006 (GPIO38; /led /state /clear /quit /help)");
    esp_err_t led_err = agent_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "RGB initialization failed: %s", esp_err_to_name(led_err)); return;
    }
    puts("RGB self-test: red, green, blue, off (GPIO38). Watch the RGB LED, not PWR.");
    const char *test_colors[] = {"red", "green", "blue", "off"};
    for (unsigned i = 0; i < 4; ++i) {
        if (!agent_led_set(test_colors[i], 50)) { ESP_LOGE(TAG, "RGB self-test driver failed"); return; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Avoid verbose HTTP header logs containing Authorization. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs attention; refusing to erase existing data automatically."); return;
    }
    ESP_ERROR_CHECK(err);
    events = xEventGroupCreate();
    if (!events) { ESP_LOGE(TAG, "Event group allocation failed"); return; }
    char ssid[33], password[65], key[256];
    read_line("Wi-Fi SSID:", ssid, sizeof(ssid), false, false);
    read_line("Wi-Fi password (blank for open network):", password, sizeof(password), true, true);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi.sta.password, password, strlen(password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    memset(password, 0, sizeof(password)); memset(&wifi, 0, sizeof(wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(events, WIFI_OK | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(45000));
    if (!(bits & WIFI_OK)) { ESP_LOGE(TAG, "Wi-Fi failed; reset to retry"); return; }
    ESP_LOGI(TAG, "Wi-Fi connected; synchronizing clock for certificate verification");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org"); esp_sntp_init();
    time_t now = 0;
    for (int i = 0; i < 30; ++i) {
        time(&now); if (now > 1704067200) break; vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (now <= 1704067200) { ESP_LOGE(TAG, "NTP timeout; reset to retry"); return; }

    read_line("DeepSeek API key (masked):", key, sizeof(key), false, true);
    trim_key(key);
    if (!key[0]) { ESP_LOGE(TAG, "Empty key"); return; }
    for (size_t i = 0; key[i]; ++i) {
        if ((unsigned char)key[i] < 33 || (unsigned char)key[i] > 126) {
            memset(key, 0, sizeof(key)); ESP_LOGE(TAG, "Key contains spaces/control/non-ASCII bytes"); return;
        }
    }
    puts("Chat ready. Commands: /led COLOR, /state, /clear, /quit, /help. Blank input sends nothing.");
    char prompt[512];
    for (;;) {
        read_line("You:", prompt, sizeof(prompt), true, false);
        bool has_text = false;
        for (size_t i = 0; prompt[i]; ++i)
            if (!isspace((unsigned char)prompt[i])) { has_text = true; break; }
        if (!has_text) continue;
        if (strcmp(prompt, "/quit") == 0) break;
        if (strcmp(prompt, "/clear") == 0) { history_clear(); puts("History cleared."); continue; }
        if (strcmp(prompt, "/state") == 0) {
            char state[128]; agent_led_state(state, sizeof(state)); puts(state); continue;
        }
        if (strncmp(prompt, "/led ", 5) == 0) {
            bool changed = agent_led_set(prompt + 5, 50);
            puts(changed ? "Local LED command OK (no API request)." : "LED command failed. Use /help.");
            continue;
        }
        if (strcmp(prompt, "/help") == 0) {
            puts("/led red|green|blue|yellow|cyan|purple|white|off; /state; /clear; /quit. Max 4 turns, 12 KiB. /clear does not change LED."); continue;
        }
        if (prompt[0] == '/') { puts("Unknown command. Use /help."); continue; }
        if (!(xEventGroupGetBits(events) & WIFI_OK)) {
            puts("Wi-Fi is not connected. This question was not sent; reset if reconnection fails."); continue;
        }
        bool ok = ask_deepseek(key, prompt);
        ESP_LOGI(TAG, "%s", ok ? "TURN PASS" : "TURN FAIL; history unchanged; no automatic resend");
    }
    history_clear();
    if (!agent_led_set("off", 0)) ESP_LOGW(TAG, "Could not switch LED off");
    volatile unsigned char *secret = (volatile unsigned char *)key;
    for (size_t i = 0; i < sizeof(key); ++i) secret[i] = 0;
    puts("Chat ended; local history and key cleared. RESET to start again.");
}
