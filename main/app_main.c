#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <stdint.h>
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
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "agent_led.h"
#define WIFI_OK BIT0
#define WIFI_STARTED BIT1
#define BODY_CAPACITY (64 * 1024)
static const char *TAG = "deepseek_v4";
static EventGroupHandle_t events;
static unsigned retries;
static atomic_uint network_epoch;
static esp_timer_handle_t reconnect_timer;
ESP_EVENT_DEFINE_BASE(AGENT_NET_EVENT);
/* Only the default event-loop task changes these reconnect state variables. */
static bool connecting;
static int64_t next_connect_at, connect_deadline;
static bool key_saved;
static unsigned turn_number, tool_actions;
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
static void schedule_reconnect(void) {
    unsigned delay = retries < 5 ? (1u << retries) : 30u;
    if (retries < 5) ++retries;
    connecting = false;
    next_connect_at = esp_timer_get_time() + (int64_t)delay * 1000000;
    ESP_LOGW(TAG, "Wi-Fi retry in %u s; automatic reconnect stays enabled", delay);
}
static void reconnect_tick(void *arg) {
    (void)arg;
    /* Timer never performs network I/O. A dropped tick is retried next second. */
    (void)esp_event_post(AGENT_NET_EVENT, 0, NULL, 0, 0);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(events, WIFI_STARTED);
        retries = 0; connecting = false; next_connect_at = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(events, WIFI_OK | WIFI_STARTED);
        atomic_fetch_add(&network_epoch, 1); connecting = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(events, WIFI_OK);
        atomic_fetch_add(&network_epoch, 1);
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u; queued API requests=0",
                 d ? (unsigned)d->reason : 0u);
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(events, WIFI_OK);
        atomic_fetch_add(&network_epoch, 1);
        (void)esp_wifi_disconnect();
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        retries = 0; connecting = false;
        xEventGroupSetBits(events, WIFI_OK);
        ESP_LOGI(TAG, "Wi-Fi ONLINE; ready for NEW input; no request replay");
    }
    if (base != AGENT_NET_EVENT) return;
    EventBits_t bits = xEventGroupGetBits(events);
    if (!(bits & WIFI_STARTED) || (bits & WIFI_OK)) return;
    int64_t now = esp_timer_get_time();
    if (connecting) {
        if (now >= connect_deadline) {
            ESP_LOGW(TAG, "Wi-Fi connection/DHCP timeout; restarting attempt");
            (void)esp_wifi_disconnect(); schedule_reconnect();
        }
        return;
    }
    if (now < next_connect_at) return;
    ESP_LOGI(TAG, "Wi-Fi connecting...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        connecting = true; connect_deadline = now + 30000000;
    } else {
        ESP_LOGW(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
        schedule_reconnect();
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
static void wipe_secret(void *buffer, size_t size) {
    volatile unsigned char *p = buffer;
    while (size--) *p++ = 0;
}
static bool valid_key(const char *key) {
    if (!key[0]) return false;
    for (size_t i = 0; key[i]; ++i)
        if ((unsigned char)key[i] < 33 || (unsigned char)key[i] > 126) return false;
    return true;
}
static esp_err_t key_load(char *key, size_t cap) {
    nvs_handle_t handle;
    wipe_secret(key, cap);
    esp_err_t err = nvs_open("deepseek_agent", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = cap;
    err = nvs_get_str(handle, "api_key", key, &size);
    nvs_close(handle);
    if (err != ESP_OK || !valid_key(key)) {
        wipe_secret(key, cap);
        return err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
    }
    return ESP_OK;
}
static esp_err_t key_store(const char *key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("deepseek_agent", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "api_key", key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
static esp_err_t key_forget(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("deepseek_agent", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, "api_key");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
static void key_prompt(char *key, size_t cap) {
    char input[256];
    for (;;) {
        read_line("DeepSeek API key (masked; saved on this board; blank cancels):",
                  input, sizeof(input), true, true);
        trim_key(input);
        if (!input[0]) { wipe_secret(input, sizeof(input)); return; }
        if (!valid_key(input)) { puts("Invalid key characters; enter again."); continue; }
        esp_err_t err = key_store(input);
        wipe_secret(key, cap);
        snprintf(key, cap, "%s", input);
        wipe_secret(input, sizeof(input));
        key_saved = err == ESP_OK;
        if (key_saved) puts("API key saved in NVS. Future boots will reuse it.");
        else ESP_LOGW(TAG, "Key usable in RAM only; NVS save failed: %s. Old stored value may remain.", esp_err_to_name(err));
        return;
    }
}
static bool cloud_ready(void) {
    if (!(xEventGroupGetBits(events) & WIFI_OK)) {
        puts("OFFLINE: input discarded, not queued. Wi-Fi reconnect continues; enter a NEW request after ONLINE.");
        return false;
    }
    time_t now; time(&now);
    if (now <= 1704067200) {
        puts("Clock not synchronized yet; NTP continues. Request not sent; try again later.");
        return false;
    }
    return true;
}
static void show_error(int status) {
    switch (status) {
        case 400: ESP_LOGE(TAG, "Request rejected: check model and JSON parameters"); break;
        case 401: ESP_LOGE(TAG, "Authentication rejected: use /key set to replace the stored DeepSeek key"); break;
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
    if (!cloud_ready()) return NULL;
    unsigned request_epoch = atomic_load(&network_epoch);
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
    if (request_epoch != atomic_load(&network_epoch) ||
        !(xEventGroupGetBits(events) & WIFI_OK))
        ESP_LOGW(TAG, "Connection changed during request; response discarded; no retry");
    else if (response.overflow) ESP_LOGE(TAG, "Response exceeded 64 KiB limit");
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

/* Read-only snapshot: never include credentials, SSID, MAC, or IP. */
static bool device_status(const char *arguments, char *result, size_t cap) {
    cJSON *args = arguments && strlen(arguments) <= 64
        ? cJSON_ParseWithOpts(arguments, NULL, true) : NULL;
    bool valid = cJSON_IsObject(args) && args->child == NULL;
    cJSON_Delete(args);
    if (!valid) {
        snprintf(result, cap, "{\"ok\":false,\"error\":\"expected_empty_object\"}");
        return false;
    }
    bool online = (xEventGroupGetBits(events) & WIFI_OK) != 0;
    wifi_ap_record_t ap;
    char rssi[16] = "null";
    if (online && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        snprintf(rssi, sizeof(rssi), "%d", (int)ap.rssi);
    char led[128]; agent_led_state(led, sizeof(led));
    /* led is generated from firmware constants, never from user input. */
    int n = snprintf(result, cap,
        "{\"ok\":true,\"firmware\":\"DEEPSEEK-V4-STATUS-20261006\","
        "\"uptime_ms\":%lld,\"wifi_online\":%s,\"rssi_dbm\":%s,"
        "\"free_internal_bytes\":%u,\"free_psram_bytes\":%u,"
        "\"led_commanded_state\":\"%s\",\"successful_led_actions\":%u}",
        (long long)(esp_timer_get_time()/1000), online ? "true" : "false", rssi,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        led, tool_actions);
    if (n < 0 || (size_t)n >= cap) {
        snprintf(result, cap, "{\"ok\":false,\"error\":\"status_buffer_too_small\"}");
        return false;
    }
    return true;
}

static const char *tool_schema =
    "[{\"type\":\"function\",\"function\":{\"name\":\"set_led\","
    "\"description\":\"Set onboard RGB LED. One static color per user turn; no blinking.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"color\":{\"type\":\"string\",\"enum\":[\"off\",\"red\",\"green\",\"blue\",\"yellow\",\"cyan\",\"purple\",\"white\"]},"
    "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"color\",\"brightness\"],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"get_device_status\","
    "\"description\":\"Read current device uptime, Wi-Fi RSSI, free RAM, and commanded LED state.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}}]";

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
    char state[128], system[1280];
    agent_led_state(state, sizeof(state));
    snprintf(system, sizeof(system),
        "You are an ESP32 assistant. Reply briefly in the user's language. "
        "For an explicit request to change this board's LED, call set_led exactly once. "
        "Never claim a change without a successful tool result. Brightness defaults to 50. "
        "Only static colors are supported. No timers, blinking or other GPIO control. "
        "For multiple colors or sequences ask the user to choose one. "
        "Current firmware state: %s. This state supersedes earlier chat. "
        "No physical light sensor is present. "
        "For questions about current device status, Wi-Fi signal, uptime, free memory, or LED state, "
        "call get_device_status with {}. Never invent telemetry or reuse an old snapshot. "
        "Report RSSI in dBm and memory in bytes or KiB (1024 bytes). Null RSSI means unavailable. "
        "Only one tool call per user turn; ask users to split combined change-and-status requests. "
        "Wi-Fi online means an IP was obtained, not a guarantee of internet access.", state);
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
        puts("Tool request rejected: only one tool call per turn. Split action and status requests. No tool executed.");
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
        char result[1024];
        const char *tool_name = "unknown";
        if (!strcmp(name->valuestring, "set_led")) {
            tool_name = "set_led";
            acted = agent_led_execute(name->valuestring, args->valuestring, result, sizeof(result));
            if (acted) ++tool_actions;
        } else if (!strcmp(name->valuestring, "get_device_status")) {
            tool_name = "get_device_status";
            (void)device_status(args->valuestring, result, sizeof(result));
        } else {
            snprintf(result, sizeof(result), "{\"ok\":false,\"error\":\"unknown_tool\"}");
        }
        printf("TOOL %s: %s; turn=%u; successful_tool_actions=%u\n", tool_name, result, turn_number, tool_actions);
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
    puts("\nFirmware: DEEPSEEK-V4-STATUS-20261006 (GPIO38; persistent key; automatic Wi-Fi reconnect)");
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
    char ssid[33], password[65], key[256] = {0};
    esp_err_t key_err = key_load(key, sizeof(key));
    key_saved = key_err == ESP_OK;
    if (key_saved) puts("Saved API key loaded (hidden). Use /key set to change, /key clear to forget.");
    else {
        if (key_err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "Saved key unavailable: %s", esp_err_to_name(key_err));
        key_prompt(key, sizeof(key));
    }
    read_line("Wi-Fi SSID:", ssid, sizeof(ssid), false, false);
    read_line("Wi-Fi password (blank for open network):", password, sizeof(password), true, true);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(AGENT_NET_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    esp_timer_create_args_t timer_args = {.callback = reconnect_tick, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(reconnect_timer, 1000000));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi.sta.password, password, strlen(password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    memset(password, 0, sizeof(password)); memset(&wifi, 0, sizeof(wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org"); esp_sntp_init();
    puts("Chat ready; Wi-Fi and NTP run in background. /status /device /key set /key clear /led COLOR /state /clear /quit /help");
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
        if (strcmp(prompt, "/device") == 0) {
            char result[1024]; (void)device_status("{}", result, sizeof(result));
            puts(result); continue;
        }
        if (strcmp(prompt, "/status") == 0) {
            time_t now; time(&now);
            printf("Wi-Fi=%s; clock=%s; key=%s; saved=%s; turns_started=%u; successful_tool_actions=%u\n",
                   (xEventGroupGetBits(events) & WIFI_OK) ? "ONLINE" : "OFFLINE",
                   now > 1704067200 ? "ready" : "waiting", key[0] ? "loaded" : "missing",
                   key_saved ? "yes" : "no", turn_number, tool_actions);
            continue;
        }
        if (strcmp(prompt, "/key set") == 0) { key_prompt(key, sizeof(key)); continue; }
        if (strcmp(prompt, "/key clear") == 0) {
            esp_err_t result = key_forget();
            if (result == ESP_OK) {
                wipe_secret(key, sizeof(key)); key_saved = false;
                puts("Stored key removed and RAM key cleared. Use /key set before cloud chat.");
            } else ESP_LOGE(TAG, "Key removal failed: %s; RAM key retained", esp_err_to_name(result));
            continue;
        }
        if (strcmp(prompt, "/help") == 0) {
            puts("/led red|green|blue|yellow|cyan|purple|white|off; /state; /status; /device; /key set; /key clear; /clear; /quit. /quit preserves saved key."); continue;
        }
        if (prompt[0] == '/') { puts("Unknown command. Use /help."); continue; }
        if (!key[0]) { puts("No API key. Use /key set."); continue; }
        if (!cloud_ready()) continue;
        ++turn_number;
        ESP_LOGI(TAG, "Turn %u started; no automatic request retry", turn_number);
        bool ok = ask_deepseek(key, prompt);
        ESP_LOGI(TAG, "%s", ok ? "TURN PASS" : "TURN FAIL; history unchanged; no automatic resend");
    }
    history_clear();
    if (!agent_led_set("off", 0)) ESP_LOGW(TAG, "Could not switch LED off");
    wipe_secret(key, sizeof(key));
    ESP_ERROR_CHECK(esp_timer_stop(reconnect_timer));
    ESP_ERROR_CHECK(esp_wifi_stop());
    puts("Chat ended; RAM history/key cleared. Saved NVS key retained. RESET to start again.");
}
