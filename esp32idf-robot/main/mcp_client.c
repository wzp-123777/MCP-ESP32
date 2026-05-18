#include "mcp_client.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_ui.h"
#include "audio_player.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef enum {
    MCP_STATUS_NOT_CONFIGURED,
    MCP_STATUS_CONFIGURED,
    MCP_STATUS_WIFI_CONNECTING,
    MCP_STATUS_WIFI_CONNECTED,
    MCP_STATUS_CONNECTING,
    MCP_STATUS_CONNECTED,
    MCP_STATUS_DISCONNECTED,
    MCP_STATUS_ERROR,
} mcp_status_t;

typedef enum {
    TTS_PLAY_ITEM_WAV,
    TTS_PLAY_ITEM_PCM,
    TTS_PLAY_ITEM_END,
} tts_play_item_kind_t;

typedef enum {
    AUDIO_UPLOAD_ITEM_START,
    AUDIO_UPLOAD_ITEM_CHUNK,
    AUDIO_UPLOAD_ITEM_END,
} audio_upload_item_kind_t;

typedef struct {
    tts_play_item_kind_t kind;
    uint8_t *data;
    size_t len;
    uint32_t segment_no;
    bool preroll;
} tts_play_item_t;

typedef struct {
    audio_upload_item_kind_t kind;
    char session_id[32];
    uint8_t *data;
    size_t len;
    uint32_t duration_ms;
    char reason[32];
} audio_upload_item_t;

typedef enum {
    MCP_UI_EVENT_STATUS,
    MCP_UI_EVENT_TEXT,
    MCP_UI_EVENT_ERROR,
    MCP_UI_EVENT_CONNECTED,
    MCP_UI_EVENT_DISCONNECTED,
} mcp_ui_event_type_t;

typedef struct {
    mcp_ui_event_type_t type;
    app_ui_assistant_state_t state;
    char text[128];
} mcp_ui_event_t;

static const char *TAG = "MCP_CLIENT";

typedef struct {
    const char *ssid;
    const char *password;
    const char *endpoint;
    uint8_t endpoint_host_octet;
} wifi_profile_t;

#ifndef ROBOT_WIFI_PROFILES
#define ROBOT_WIFI_PROFILES \
    { \
        {ROBOT_WIFI_SSID, ROBOT_WIFI_PASSWORD, ROBOT_MCP_URI, 0}, \
    }
#endif

static const wifi_profile_t s_wifi_profiles[] = ROBOT_WIFI_PROFILES;
#define WIFI_PROFILE_COUNT (sizeof(s_wifi_profiles) / sizeof(s_wifi_profiles[0]))
#define WIFI_PROFILE_MAX_FAILURES 2

static char s_endpoint[160] = ROBOT_MCP_URI;
static mcp_status_t s_status = MCP_STATUS_NOT_CONFIGURED;
static EventGroupHandle_t s_event_group;
static SemaphoreHandle_t s_send_lock;
static QueueHandle_t s_tts_play_queue;
static QueueHandle_t s_audio_upload_queue;
static QueueHandle_t s_ui_event_queue;
static esp_websocket_client_handle_t s_ws;
static bool s_netif_ready;
static bool s_wifi_connected;
static bool s_ws_connected;
static bool s_started;
static bool s_endpoint_user_override;
static size_t s_wifi_profile_index;
static uint8_t s_wifi_profile_failures;
static esp_netif_ip_info_t s_last_ip_info;
static bool s_last_ip_info_valid;
static bool s_sr_enabled;
static bool s_ws_restart_requested;
static bool s_ws_recreate_requested;
static bool s_ws_stopping;
static bool s_sntp_started;
static TickType_t s_last_ws_start_tick;
static char *s_rx_buffer;
static size_t s_rx_cap;
static size_t s_rx_len;
static bool s_rx_dropping;
static uint8_t *s_tts_buf;
static size_t s_tts_len;
static size_t s_tts_cap;
static char s_tts_b64_pending[4];
static size_t s_tts_b64_pending_len;
static size_t s_tts_chunk_count;
static size_t s_tts_b64_chars;
static uint32_t s_tts_segment_no;
static bool s_tts_pcm_active;
static bool s_tts_pcm_first_chunk;
static uint32_t s_tts_pcm_chunk_no;
static char s_current_session_id[32];
static app_ui_assistant_state_t s_last_ui_state = APP_UI_STATE_IDLE;
static TickType_t s_last_ui_state_tick;
static volatile bool s_assistant_busy;
static bool s_tts_playback_busy;
static char s_cfg_persona_id[48] = "default";
static char s_cfg_persona_label[64] = "默认人设";
static char s_cfg_voice_id[48] = "default";
static char s_cfg_voice_label[64] = "默认音色";
static bool s_cfg_continuous_chat;
static bool s_cfg_wake_enabled;
static bool s_cfg_valid;
static mcp_client_busy_cb_t s_busy_cb;
static void *s_busy_ctx;
static mcp_client_playback_cb_t s_playback_cb;
static void *s_playback_ctx;
static mcp_client_device_command_cb_t s_device_command_cb;
static void *s_device_command_ctx;

#define MCP_WIFI_CONNECTED_BIT BIT0
#define MCP_SEND_TIMEOUT pdMS_TO_TICKS(3000)
#define MCP_MAX_RX_MESSAGE (256 * 1024)
#define MCP_MAX_TTS_BYTES (384 * 1024)
#define MCP_MIN_TTS_BYTES 1024
#define MCP_TTS_PLAY_QUEUE_LEN 12
#define MCP_AUDIO_UPLOAD_QUEUE_LEN 64
#define MCP_AUDIO_UPLOAD_BEGIN_WAIT_MS 250
#define MCP_AUDIO_UPLOAD_END_WAIT_MS 250
#define MCP_TTS_QUEUE_WAIT_MS 250
#define MCP_UI_EVENT_QUEUE_LEN 8
#define MCP_ENDPOINT_NVS_NAMESPACE "mcp_robot"
#define MCP_ENDPOINT_NVS_KEY "endpoint"
#define MCP_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MCP_QUEUE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void websocket_restart(void);
static esp_err_t websocket_create_and_start(void);
static void websocket_request_restart(void);
static void websocket_request_recreate(void);
static esp_err_t load_endpoint_from_nvs(void);
static esp_err_t save_endpoint_to_nvs(const char *endpoint);
static esp_err_t wifi_apply_profile(size_t index);
static void endpoint_apply_wifi_profile(const esp_netif_ip_info_t *ip_info);
static void tts_play_task(void *arg);
static void audio_upload_task(void *arg);
static void ui_event_task(void *arg);
static const char *json_get_string(const char *json, const char *key, char *out, size_t out_size);
static bool json_get_uint32(const char *json, const char *key, uint32_t *out);
static esp_err_t send_config_payload(const char *persona_id,
                                     const char *persona_label,
                                     const char *voice_id,
                                     const char *voice_label,
                                     bool continuous_chat,
                                     bool wake_enabled);

static void log_heap_state(const char *phase)
{
    ESP_LOGI(TAG,
             "%s heap internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             phase ? phase : "mcp",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static esp_err_t create_spiram_task(TaskFunction_t task_func,
                                    const char *name,
                                    uint32_t stack_bytes,
                                    UBaseType_t prio)
{
    BaseType_t ok = xTaskCreateWithCaps(task_func, name, stack_bytes, NULL, prio, NULL, MCP_TASK_STACK_CAPS);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed name=%s stack=%u", name ? name : "?", (unsigned)stack_bytes);
        log_heap_state("task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void set_assistant_busy(bool busy)
{
    if (s_assistant_busy == busy) {
        return;
    }
    s_assistant_busy = busy;
    if (s_busy_cb) {
        s_busy_cb(busy, s_busy_ctx);
    }
}

static void set_tts_playback_busy(bool busy)
{
    if (s_tts_playback_busy == busy) {
        return;
    }
    s_tts_playback_busy = busy;
    if (s_playback_cb) {
        s_playback_cb(busy, s_playback_ctx);
    }
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    setenv("TZ", "CST-8", 1);
    tzset();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_setservername(1, "cn.pool.ntp.org");
    sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started tz=UTC+8");
}

static app_ui_assistant_state_t assistant_state_from_status(const char *status)
{
    if (!status) {
        return APP_UI_STATE_IDLE;
    }
    if (strcmp(status, "listening") == 0 || strcmp(status, "recording") == 0) {
        return APP_UI_STATE_RECORDING;
    }
    if (strcmp(status, "uploading") == 0) {
        return APP_UI_STATE_UPLOADING;
    }
    if (strcmp(status, "asr") == 0 || strcmp(status, "asr_started") == 0) {
        return APP_UI_STATE_ASR;
    }
    if (strcmp(status, "thinking") == 0) {
        return APP_UI_STATE_THINKING;
    }
    if (strcmp(status, "tts") == 0) {
        return APP_UI_STATE_TTS;
    }
    if (strcmp(status, "playing") == 0) {
        return APP_UI_STATE_PLAYING;
    }
    if (strcmp(status, "error") == 0) {
        return APP_UI_STATE_ERROR;
    }
    return APP_UI_STATE_IDLE;
}

static void ui_post_event(mcp_ui_event_type_t type, app_ui_assistant_state_t state, const char *text)
{
    if (!s_ui_event_queue) {
        return;
    }
    mcp_ui_event_t event = {
        .type = type,
        .state = state,
    };
    if (text) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    if (xQueueSend(s_ui_event_queue, &event, 0) != pdTRUE) {
        mcp_ui_event_t dropped = {0};
        xQueueReceive(s_ui_event_queue, &dropped, 0);
        xQueueSend(s_ui_event_queue, &event, 0);
    }
}

static bool message_is_for_current_session(const char *message)
{
    char session_id[32];
    if (!json_get_string(message, "session_id", session_id, sizeof(session_id))) {
        return true;
    }
    if (session_id[0] == '\0' || s_current_session_id[0] == '\0') {
        return true;
    }
    return strcmp(session_id, s_current_session_id) == 0;
}

static bool message_is_for_this_device(const char *message)
{
    char device_id[48];
    if (!json_get_string(message, "device_id", device_id, sizeof(device_id))) {
        return true;
    }
    return device_id[0] == '\0' || strcmp(device_id, ROBOT_DEVICE_ID) == 0;
}

static int assistant_state_priority(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_ERROR:
            return 90;
        case APP_UI_STATE_RECORDING:
            return 80;
        case APP_UI_STATE_PLAYING:
            return 70;
        case APP_UI_STATE_TTS:
            return 60;
        case APP_UI_STATE_THINKING:
            return 50;
        case APP_UI_STATE_ASR:
            return 40;
        case APP_UI_STATE_UPLOADING:
            return 30;
        case APP_UI_STATE_OFFLINE:
            return 20;
        case APP_UI_STATE_IDLE:
        default:
            return 10;
    }
}

static bool should_apply_ui_state(app_ui_assistant_state_t next)
{
    TickType_t now = xTaskGetTickCount();
    if (next == APP_UI_STATE_ERROR || next == APP_UI_STATE_RECORDING || next == APP_UI_STATE_OFFLINE) {
        return true;
    }
    if (next == APP_UI_STATE_IDLE && s_tts_pcm_active) {
        return false;
    }
    if (next == APP_UI_STATE_IDLE &&
        (s_last_ui_state == APP_UI_STATE_PLAYING || s_last_ui_state == APP_UI_STATE_TTS) &&
        now - s_last_ui_state_tick < pdMS_TO_TICKS(2500)) {
        return false;
    }
    if (assistant_state_priority(next) < assistant_state_priority(s_last_ui_state) &&
        now - s_last_ui_state_tick < pdMS_TO_TICKS(800)) {
        return false;
    }
    return true;
}

static const char *json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return NULL;
    }
    out[0] = '\0';

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return NULL;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    ++p;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n':
                    out[n++] = '\n';
                    break;
                case 'r':
                    out[n++] = '\r';
                    break;
                case 't':
                    out[n++] = '\t';
                    break;
                case '"':
                case '\\':
                case '/':
                    out[n++] = *p;
                    break;
                default:
                    out[n++] = *p;
                    break;
            }
            ++p;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return out;
}

static bool json_get_string_span(const char *json, const char *key, const char **out, size_t *out_len)
{
    if (!json || !key || !out || !out_len) {
        return false;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '"') {
        return false;
    }
    ++p;

    const char *start = p;
    while (*p) {
        if (*p == '\\') {
            return false;
        }
        if (*p == '"') {
            *out = start;
            *out_len = (size_t)(p - start);
            return true;
        }
        ++p;
    }
    return false;
}

static bool json_get_uint32(const char *json, const char *key, uint32_t *out)
{
    if (!json || !key || !out) {
        return false;
    }

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    unsigned long value = strtoul(p, NULL, 10);
    if (value > UINT32_MAX) {
        value = UINT32_MAX;
    }
    *out = (uint32_t)value;
    return true;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t n = 0;
    for (const char *p = in ? in : ""; *p && n + 1 < out_size; ++p) {
        char ch = *p;
        if ((ch == '"' || ch == '\\') && n + 2 < out_size) {
            out[n++] = '\\';
            out[n++] = ch;
        } else if (ch == '\n' && n + 2 < out_size) {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (ch == '\r' && n + 2 < out_size) {
            out[n++] = '\\';
            out[n++] = 'r';
        } else {
            out[n++] = ch;
        }
    }
    out[n] = '\0';
}

static esp_err_t ws_send_json(const char *payload)
{
    if (!payload || !s_ws_connected || !s_ws) {
        ESP_LOGW(TAG, "send skipped: ws disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_send_lock, MCP_SEND_TIMEOUT) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int sent = esp_websocket_client_send_text(s_ws, payload, strlen(payload), MCP_SEND_TIMEOUT);
    xSemaphoreGive(s_send_lock);

    if (sent < 0) {
        ESP_LOGW(TAG, "send failed");
        s_ws_connected = false;
        websocket_request_restart();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void websocket_request_restart(void)
{
    s_ws_restart_requested = true;
}

static void websocket_request_recreate(void)
{
    s_ws_recreate_requested = true;
    s_ws_restart_requested = true;
}

static esp_err_t load_endpoint_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MCP_ENDPOINT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char stored[sizeof(s_endpoint)] = {0};
    size_t len = sizeof(stored);
    err = nvs_get_str(handle, MCP_ENDPOINT_NVS_KEY, stored, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (stored[0]) {
        strlcpy(s_endpoint, stored, sizeof(s_endpoint));
        s_endpoint_user_override = true;
        ESP_LOGI(TAG, "endpoint loaded from nvs=%s", s_endpoint);
    }
    return ESP_OK;
}

static esp_err_t save_endpoint_to_nvs(const char *endpoint)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MCP_ENDPOINT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (!endpoint || endpoint[0] == '\0') {
        err = nvs_erase_key(handle, MCP_ENDPOINT_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, MCP_ENDPOINT_NVS_KEY, endpoint);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool rx_reserve(size_t needed)
{
    if (needed <= s_rx_cap) {
        return true;
    }
    if (needed > MCP_MAX_RX_MESSAGE) {
        return false;
    }

    size_t new_cap = s_rx_cap ? s_rx_cap * 2 : 1024;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    if (new_cap > MCP_MAX_RX_MESSAGE) {
        new_cap = needed;
    }

    char *next = heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        next = heap_caps_malloc(new_cap, MALLOC_CAP_8BIT);
    }
    if (!next) {
        return false;
    }

    if (s_rx_buffer && s_rx_len > 0) {
        memcpy(next, s_rx_buffer, s_rx_len);
    }
    if (s_rx_buffer) {
        heap_caps_free(s_rx_buffer);
    }
    s_rx_buffer = next;
    s_rx_cap = new_cap;
    return true;
}

static void tts_reset(void)
{
    if (s_tts_buf) {
        heap_caps_free(s_tts_buf);
    }
    s_tts_buf = NULL;
    s_tts_len = 0;
    s_tts_cap = 0;
    s_tts_b64_pending_len = 0;
    s_tts_chunk_count = 0;
    s_tts_b64_chars = 0;
}

static void tts_drop_queued_items(void)
{
    if (!s_tts_play_queue) {
        return;
    }
    tts_play_item_t item = {0};
    while (xQueueReceive(s_tts_play_queue, &item, 0) == pdTRUE) {
        if (item.data) {
            heap_caps_free(item.data);
            item.data = NULL;
        }
    }
}

static void audio_upload_drop_queued_items(void)
{
    if (!s_audio_upload_queue) {
        return;
    }
    audio_upload_item_t item = {0};
    size_t dropped = 0;
    while (xQueueReceive(s_audio_upload_queue, &item, 0) == pdTRUE) {
        if (item.data) {
            heap_caps_free(item.data);
            item.data = NULL;
        }
        ++dropped;
    }
    if (dropped > 0) {
        ESP_LOGW(TAG, "audio upload queue cleared items=%u", (unsigned)dropped);
    }
}

void mcp_client_cancel_playback(void)
{
    audio_player_cancel();
    tts_reset();
    tts_drop_queued_items();
    s_tts_pcm_active = false;
    s_tts_pcm_first_chunk = false;
    set_tts_playback_busy(false);
    set_assistant_busy(false);
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
    ESP_LOGI(TAG, "tts playback canceled and queue cleared");
}

static bool audio_upload_queue_item(audio_upload_item_t *item, TickType_t wait_ticks, const char *label)
{
    if (!item || !s_audio_upload_queue) {
        return false;
    }
    if (!s_ws_connected) {
        if (item->data) {
            heap_caps_free(item->data);
            item->data = NULL;
        }
        ESP_LOGW(TAG, "%s skipped: ws disconnected", label ? label : "audio upload");
        return false;
    }
    if (xQueueSend(s_audio_upload_queue, item, wait_ticks) == pdTRUE) {
        UBaseType_t queued = uxQueueMessagesWaiting(s_audio_upload_queue);
        if (queued >= (MCP_AUDIO_UPLOAD_QUEUE_LEN * 3) / 4) {
            ESP_LOGW(TAG, "audio upload queue high queued=%u/%u", (unsigned)queued, MCP_AUDIO_UPLOAD_QUEUE_LEN);
        }
        return true;
    }
    if (item->data) {
        heap_caps_free(item->data);
        item->data = NULL;
    }
    ESP_LOGW(TAG, "%s queue full, dropped len=%u", label ? label : "audio upload", (unsigned)item->len);
    return false;
}

static esp_err_t audio_upload_send_start(const audio_upload_item_t *item)
{
    char payload[256];
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"audio_stream_start\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"sample_rate\":%d,\"sample_bits\":%d,\"channels\":%d,\"encoding\":\"pcm_s16le\"}",
             ROBOT_DEVICE_ID,
             item ? item->session_id : "",
             ROBOT_AUDIO_SAMPLE_RATE,
             ROBOT_AUDIO_BITS,
             ROBOT_AUDIO_CHANNELS);
    return ws_send_json(payload);
}

static esp_err_t audio_upload_send_chunk(audio_upload_item_t *item)
{
    if (!item || !item->data || item->len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t b64_cap = ((item->len + 2) / 3) * 4 + 1;
    char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64) {
        b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_8BIT);
    }
    if (!b64) {
        return ESP_ERR_NO_MEM;
    }

    size_t b64_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len, item->data, item->len);
    if (rc != 0) {
        heap_caps_free(b64);
        return ESP_FAIL;
    }
    b64[b64_len] = '\0';

    size_t payload_cap = b64_len + 160;
    char *payload = heap_caps_malloc(payload_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
        payload = heap_caps_malloc(payload_cap, MALLOC_CAP_8BIT);
    }
    if (!payload) {
        heap_caps_free(b64);
        return ESP_ERR_NO_MEM;
    }
    snprintf(payload,
             payload_cap,
             "{\"type\":\"audio_stream_chunk\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"audio_b64\":\"%s\"}",
             ROBOT_DEVICE_ID,
             item->session_id,
             b64);
    esp_err_t err = ws_send_json(payload);
    heap_caps_free(payload);
    heap_caps_free(b64);
    return err;
}

static esp_err_t audio_upload_send_end(const audio_upload_item_t *item)
{
    char reason_escaped[48];
    char payload[256];
    json_escape(item ? item->reason : "", reason_escaped, sizeof(reason_escaped));
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"audio_stream_end\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"duration_ms\":%u,\"reason\":\"%s\"}",
             ROBOT_DEVICE_ID,
             item ? item->session_id : "",
             (unsigned)(item ? item->duration_ms : 0),
             reason_escaped[0] ? reason_escaped : "set_release");
    return ws_send_json(payload);
}

static void audio_upload_task(void *arg)
{
    (void)arg;
    audio_upload_item_t item = {0};
    while (true) {
        if (xQueueReceive(s_audio_upload_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t ret = ESP_OK;
        switch (item.kind) {
            case AUDIO_UPLOAD_ITEM_START:
                ret = audio_upload_send_start(&item);
                break;
            case AUDIO_UPLOAD_ITEM_CHUNK:
                ret = audio_upload_send_chunk(&item);
                break;
            case AUDIO_UPLOAD_ITEM_END:
                ret = audio_upload_send_end(&item);
                break;
        }
        if (item.data) {
            heap_caps_free(item.data);
            item.data = NULL;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "audio upload send failed kind=%d session=%s ret=%s",
                     item.kind,
                     item.session_id,
                     esp_err_to_name(ret));
        }
    }
}

static bool tts_reserve(size_t needed)
{
    if (needed > MCP_MAX_TTS_BYTES) {
        ESP_LOGW(TAG, "tts buffer too large: needed=%u max=%u", (unsigned)needed, (unsigned)MCP_MAX_TTS_BYTES);
        return false;
    }
    if (needed <= s_tts_cap) {
        return true;
    }

    size_t new_cap = s_tts_cap ? s_tts_cap * 2 : 8192;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    uint8_t *next = heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        next = heap_caps_malloc(new_cap, MALLOC_CAP_8BIT);
    }
    if (!next) {
        return false;
    }

    if (s_tts_buf && s_tts_len) {
        memcpy(next, s_tts_buf, s_tts_len);
    }
    if (s_tts_buf) {
        heap_caps_free(s_tts_buf);
    }
    s_tts_buf = next;
    s_tts_cap = new_cap;
    return true;
}

static bool decode_base64_alloc_span(const char *audio_b64, size_t b64_len, uint8_t **out, size_t *out_len)
{
    if (!audio_b64 || b64_len == 0 || !out || !out_len) {
        return false;
    }
    *out = NULL;
    *out_len = 0;

    size_t tmp_cap = ((b64_len + 3) / 4) * 3 + 8;
    uint8_t *tmp = heap_caps_malloc(tmp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = heap_caps_malloc(tmp_cap, MALLOC_CAP_8BIT);
    }
    if (!tmp) {
        return false;
    }

    size_t decoded_len = 0;
    int rc = mbedtls_base64_decode(tmp, tmp_cap, &decoded_len, (const unsigned char *)audio_b64, b64_len);
    if (rc != 0 || decoded_len == 0) {
        ESP_LOGW(TAG, "pcm base64 decode failed rc=%d len=%u", rc, (unsigned)b64_len);
        heap_caps_free(tmp);
        return false;
    }

    *out = tmp;
    *out_len = decoded_len;
    return true;
}

static bool tts_queue_item(tts_play_item_t *item, const char *label)
{
    TickType_t wait_ticks = pdMS_TO_TICKS(MCP_TTS_QUEUE_WAIT_MS);
    if (item && item->kind == TTS_PLAY_ITEM_WAV) {
        wait_ticks = pdMS_TO_TICKS(500);
    }
    if (!item || !s_tts_play_queue || xQueueSend(s_tts_play_queue, item, wait_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "%s queue full, dropped len=%u", label ? label : "tts", item ? (unsigned)item->len : 0);
        if (item && item->data) {
            heap_caps_free(item->data);
            item->data = NULL;
        }
        return false;
    }
    return true;
}

static bool tts_decode_base64_block(const char *audio_b64, size_t b64_len)
{
    if (!audio_b64 || b64_len == 0) {
        return true;
    }

    size_t tmp_cap = ((b64_len + 3) / 4) * 3 + 8;
    uint8_t *tmp = heap_caps_malloc(tmp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        tmp = heap_caps_malloc(tmp_cap, MALLOC_CAP_8BIT);
    }
    if (!tmp) {
        return false;
    }

    size_t out_len = 0;
    int rc = mbedtls_base64_decode(tmp, tmp_cap, &out_len, (const unsigned char *)audio_b64, b64_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "tts base64 decode failed rc=%d len=%u", rc, (unsigned)b64_len);
        heap_caps_free(tmp);
        return false;
    }

    if (!tts_reserve(s_tts_len + out_len)) {
        heap_caps_free(tmp);
        return false;
    }
    memcpy(s_tts_buf + s_tts_len, tmp, out_len);
    s_tts_len += out_len;
    heap_caps_free(tmp);
    return true;
}

static bool tts_append_base64_span(const char *audio_b64, size_t b64_len)
{
    if (!audio_b64 || b64_len == 0) {
        return true;
    }

    size_t compact_cap = s_tts_b64_pending_len + b64_len + 4;
    char *compact = heap_caps_malloc(compact_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!compact) {
        compact = heap_caps_malloc(compact_cap, MALLOC_CAP_8BIT);
    }
    if (!compact) {
        return false;
    }

    size_t compact_len = 0;
    for (size_t i = 0; i < s_tts_b64_pending_len; ++i) {
        compact[compact_len++] = s_tts_b64_pending[i];
    }
    s_tts_b64_pending_len = 0;

    for (size_t i = 0; i < b64_len; ++i) {
        unsigned char ch = (unsigned char)audio_b64[i];
        if (!isspace(ch)) {
            compact[compact_len++] = (char)ch;
        }
    }

    if (compact_len < 4) {
        memcpy(s_tts_b64_pending, compact, compact_len);
        s_tts_b64_pending_len = compact_len;
        heap_caps_free(compact);
        return true;
    }

    size_t decode_len = compact_len & ~(size_t)3;
    size_t remain = compact_len - decode_len;
    bool ok = tts_decode_base64_block(compact, decode_len);

    if (ok && remain > 0) {
        memcpy(s_tts_b64_pending, compact + decode_len, remain);
        s_tts_b64_pending_len = remain;
    }

    heap_caps_free(compact);
    return ok;
}

static bool tts_flush_base64_pending(void)
{
    if (s_tts_b64_pending_len == 0) {
        return true;
    }
    if (s_tts_b64_pending_len == 1) {
        ESP_LOGW(TAG, "invalid trailing tts base64 len=1");
        s_tts_b64_pending_len = 0;
        return false;
    }

    char padded[4] = {0};
    memcpy(padded, s_tts_b64_pending, s_tts_b64_pending_len);
    while (s_tts_b64_pending_len < 4) {
        padded[s_tts_b64_pending_len++] = '=';
    }
    s_tts_b64_pending_len = 0;
    return tts_decode_base64_block(padded, sizeof(padded));
}

static void tts_finish(void)
{
    if (!tts_flush_base64_pending()) {
        set_assistant_busy(false);
        tts_reset();
        return;
    }
    if (!s_tts_buf || s_tts_len <= 44) {
        set_assistant_busy(false);
        tts_reset();
        return;
    }
    if (s_tts_len < MCP_MIN_TTS_BYTES) {
        ESP_LOGW(TAG, "tts too small, dropped len=%u", (unsigned)s_tts_len);
        set_assistant_busy(false);
        tts_reset();
        return;
    }
    tts_play_item_t item = {
        .kind = TTS_PLAY_ITEM_WAV,
        .data = s_tts_buf,
        .len = s_tts_len,
        .segment_no = s_tts_segment_no,
    };
    s_tts_buf = NULL;
    s_tts_len = 0;
    s_tts_cap = 0;
    s_tts_b64_pending_len = 0;
    s_tts_chunk_count = 0;
    s_tts_b64_chars = 0;

    if (!tts_queue_item(&item, "tts wav")) {
        set_assistant_busy(false);
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
        return;
    }
    ESP_LOGI(TAG, "tts queued len=%u", (unsigned)item.len);
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_TTS, NULL);
}

static void tts_play_task(void *arg)
{
    (void)arg;
    tts_play_item_t item = {0};
    while (true) {
        if (xQueueReceive(s_tts_play_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.kind == TTS_PLAY_ITEM_END) {
            ESP_LOGI(TAG, "tts pcm stream end chunks=%u", (unsigned)item.segment_no);
            s_tts_pcm_active = false;
            s_tts_pcm_first_chunk = false;
            set_tts_playback_busy(false);
            set_assistant_busy(false);
            ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
            continue;
        }
        if (!item.data || item.len == 0) {
            continue;
        }
        set_assistant_busy(true);
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_PLAYING, NULL);
        if (item.kind == TTS_PLAY_ITEM_PCM) {
            set_tts_playback_busy(true);
            esp_err_t ret = audio_player_play_pcm16(item.data, item.len, "tts_pcm", item.preroll);
            ESP_LOGD(TAG,
                     "tts pcm played chunk=%u len=%u ret=%s",
                     (unsigned)item.segment_no,
                     (unsigned)item.len,
                     esp_err_to_name(ret));
            heap_caps_free(item.data);
            item.data = NULL;
            item.len = 0;
            continue;
        }

        bool first_segment = item.segment_no == 0;
        set_tts_playback_busy(true);
        esp_err_t ret = audio_player_play_wav_ex(item.data, item.len, "tts", first_segment, first_segment ? 30 : 0);
        ESP_LOGI(TAG,
                 "tts play finished segment=%u len=%u ret=%s",
                 (unsigned)item.segment_no,
                 (unsigned)item.len,
                 esp_err_to_name(ret));
        heap_caps_free(item.data);
        item.data = NULL;
        item.len = 0;
        set_tts_playback_busy(false);
        set_assistant_busy(false);
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
    }
}

static void ui_event_task(void *arg)
{
    (void)arg;
    mcp_ui_event_t event = {0};
    while (true) {
        if (xQueueReceive(s_ui_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
            case MCP_UI_EVENT_CONNECTED:
                app_ui_set_mcp_connected(true);
                app_ui_set_assistant_state(APP_UI_STATE_IDLE);
                set_assistant_busy(false);
                s_last_ui_state = APP_UI_STATE_IDLE;
                s_last_ui_state_tick = xTaskGetTickCount();
                break;
            case MCP_UI_EVENT_DISCONNECTED:
                app_ui_set_mcp_connected(false);
                set_assistant_busy(false);
                s_last_ui_state = APP_UI_STATE_OFFLINE;
                s_last_ui_state_tick = xTaskGetTickCount();
                break;
            case MCP_UI_EVENT_STATUS:
                app_ui_note_mcp_activity();
                if (event.state == APP_UI_STATE_IDLE ||
                    event.state == APP_UI_STATE_ERROR ||
                    event.state == APP_UI_STATE_OFFLINE) {
                    if (!s_tts_pcm_active) {
                        set_assistant_busy(false);
                    }
                } else if (event.state != APP_UI_STATE_RECORDING) {
                    set_assistant_busy(true);
                }
                if (should_apply_ui_state(event.state)) {
                    app_ui_set_assistant_state(event.state);
                    s_last_ui_state = event.state;
                    s_last_ui_state_tick = xTaskGetTickCount();
                }
                if (event.text[0]) {
                    app_ui_set_recent_text(event.text);
                }
                break;
            case MCP_UI_EVENT_TEXT:
                app_ui_note_mcp_activity();
                app_ui_set_recent_text(event.text);
                break;
            case MCP_UI_EVENT_ERROR:
                app_ui_note_mcp_activity();
                set_assistant_busy(false);
                app_ui_set_assistant_state(APP_UI_STATE_ERROR);
                s_last_ui_state = APP_UI_STATE_ERROR;
                s_last_ui_state_tick = xTaskGetTickCount();
                if (event.text[0]) {
                    app_ui_set_recent_text(event.text);
                }
                break;
            default:
                break;
        }
    }
}

static void handle_ws_text(const char *message)
{
    char type[40];
    char status[32];
    char text[256];
    json_get_string(message, "type", type, sizeof(type));
    app_ui_note_mcp_activity();

    if (!message_is_for_this_device(message) || !message_is_for_current_session(message)) {
        return;
    }

    if (strcmp(type, "assistant_done") == 0) {
        json_get_string(message, "text", text, sizeof(text));
        ESP_LOGI(TAG, "assistant_done: %s", text);
        ui_post_event(MCP_UI_EVENT_TEXT, APP_UI_STATE_IDLE, text);
        return;
    }

    if (strcmp(type, "assistant_status") == 0) {
        json_get_string(message, "status", status, sizeof(status));
        json_get_string(message, "text", text, sizeof(text));
        ESP_LOGI(TAG, "assistant_status: %s %s", status, text);
        ui_post_event(MCP_UI_EVENT_STATUS, assistant_state_from_status(status), text);
        return;
    }

    if (strcmp(type, "assistant_text_delta") == 0) {
        return;
    }

    if (strcmp(type, "config_ack") == 0) {
        ESP_LOGI(TAG, "config ack");
        return;
    }

    if (strcmp(type, "assistant_error") == 0 || strcmp(type, "tts_error") == 0 || strcmp(type, "vision_error") == 0) {
        json_get_string(message, "error", text, sizeof(text));
        ESP_LOGW(TAG, "%s: %s", type, text);
        if (strcmp(type, "tts_error") == 0) {
            tts_reset();
        }
        s_tts_pcm_active = false;
        s_tts_pcm_first_chunk = false;
        set_assistant_busy(false);
        ui_post_event(MCP_UI_EVENT_ERROR, APP_UI_STATE_ERROR, text);
        return;
    }

    if (strcmp(type, "tts_pcm_start") == 0) {
        uint32_t sample_rate = 16000;
        uint32_t sample_bits = 16;
        uint32_t channels = 1;
        json_get_string(message, "text", text, sizeof(text));
        json_get_uint32(message, "sample_rate", &sample_rate);
        json_get_uint32(message, "sample_bits", &sample_bits);
        json_get_uint32(message, "channels", &channels);
        tts_reset();
        s_tts_pcm_chunk_no = 0;
        s_tts_pcm_first_chunk = true;
        s_tts_pcm_active = (sample_rate == 16000 && sample_bits == 16 && channels == 1);
        set_assistant_busy(true);
        ESP_LOGI(TAG,
                 "tts pcm start rate=%u bits=%u ch=%u text=%s",
                 (unsigned)sample_rate,
                 (unsigned)sample_bits,
                 (unsigned)channels,
                 text);
        if (!s_tts_pcm_active) {
            set_assistant_busy(false);
            ui_post_event(MCP_UI_EVENT_ERROR, APP_UI_STATE_ERROR, "unsupported pcm tts format");
            return;
        }
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_TTS, text[0] ? text : NULL);
        return;
    }

    if (strcmp(type, "tts_pcm_chunk") == 0) {
        const char *audio_b64 = NULL;
        size_t audio_b64_len = 0;
        if (!s_tts_pcm_active) {
            s_tts_pcm_active = true;
            s_tts_pcm_first_chunk = true;
            s_tts_pcm_chunk_no = 0;
        }
        set_assistant_busy(true);
        if (json_get_string_span(message, "audio_b64", &audio_b64, &audio_b64_len)) {
            uint8_t *pcm = NULL;
            size_t pcm_len = 0;
            if (!decode_base64_alloc_span(audio_b64, audio_b64_len, &pcm, &pcm_len)) {
                return;
            }
            tts_play_item_t item = {
                .kind = TTS_PLAY_ITEM_PCM,
                .data = pcm,
                .len = pcm_len,
                .segment_no = s_tts_pcm_chunk_no++,
                .preroll = s_tts_pcm_first_chunk,
            };
            if (tts_queue_item(&item, "tts pcm")) {
                s_tts_pcm_first_chunk = false;
            }
        } else {
            ESP_LOGW(TAG, "tts pcm audio_b64 missing or escaped");
        }
        return;
    }

    if (strcmp(type, "tts_pcm_end") == 0) {
        tts_play_item_t item = {
            .kind = TTS_PLAY_ITEM_END,
            .segment_no = s_tts_pcm_chunk_no,
        };
        if (!tts_queue_item(&item, "tts pcm end")) {
            s_tts_pcm_active = false;
            s_tts_pcm_first_chunk = false;
            set_assistant_busy(false);
            ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
        }
        return;
    }

    if (strcmp(type, "tts_segment_start") == 0) {
        json_get_string(message, "text", text, sizeof(text));
        json_get_uint32(message, "segment_no", &s_tts_segment_no);
        ESP_LOGI(TAG, "tts segment: %s", text);
        s_tts_pcm_active = false;
        s_tts_pcm_first_chunk = false;
        set_assistant_busy(true);
        if (s_tts_len > 44 || s_tts_b64_pending_len > 0) {
            tts_finish();
        } else {
            tts_reset();
        }
        s_tts_chunk_count = 0;
        s_tts_b64_chars = 0;
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_TTS, NULL);
        return;
    }

    if (strcmp(type, "tts_audio_chunk") == 0) {
        const char *audio_b64 = NULL;
        size_t audio_b64_len = 0;
        if (json_get_string_span(message, "audio_b64", &audio_b64, &audio_b64_len)) {
            ++s_tts_chunk_count;
            s_tts_b64_chars += audio_b64_len;
            if (!tts_append_base64_span(audio_b64, audio_b64_len)) {
                ESP_LOGW(TAG, "tts base64 append failed");
                tts_reset();
            }
        } else {
            ESP_LOGW(TAG, "tts audio_b64 missing or escaped");
        }
        return;
    }

    if (strcmp(type, "tts_stream_end") == 0) {
        ESP_LOGI(TAG,
                 "tts stream end chunks=%u b64=%u bytes=%u",
                 (unsigned)s_tts_chunk_count,
                 (unsigned)s_tts_b64_chars,
                 (unsigned)s_tts_len);
        set_assistant_busy(true);
        tts_finish();
        return;
    }

    if (strcmp(type, "device_command") == 0) {
        char command[80];
        json_get_string(message, "command", command, sizeof(command));
        ESP_LOGI(TAG, "device_command: %s", command[0] ? command : message);
        if (command[0] && s_device_command_cb) {
            s_device_command_cb(command, s_device_command_ctx);
        }
        return;
    }

    ESP_LOGI(TAG, "unhandled ws: %s", message);
}

static const wifi_profile_t *wifi_current_profile(void)
{
    if (WIFI_PROFILE_COUNT == 0) {
        return NULL;
    }
    if (s_wifi_profile_index >= WIFI_PROFILE_COUNT) {
        s_wifi_profile_index = 0;
    }
    return &s_wifi_profiles[s_wifi_profile_index];
}

static esp_err_t wifi_apply_profile(size_t index)
{
    if (WIFI_PROFILE_COUNT == 0 || index >= WIFI_PROFILE_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    const wifi_profile_t *profile = &s_wifi_profiles[index];
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, profile->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, profile->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        s_wifi_profile_index = index;
        ESP_LOGI(TAG,
                 "wifi profile %u/%u ssid=%s",
                 (unsigned)(s_wifi_profile_index + 1),
                 (unsigned)WIFI_PROFILE_COUNT,
                 profile->ssid ? profile->ssid : "<empty>");
    }
    return ret;
}

static void wifi_select_next_profile(void)
{
    if (WIFI_PROFILE_COUNT <= 1) {
        return;
    }
    size_t next = (s_wifi_profile_index + 1) % WIFI_PROFILE_COUNT;
    esp_err_t ret = wifi_apply_profile(next);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi profile switch failed: %s", esp_err_to_name(ret));
    }
}

static void endpoint_apply_wifi_profile(const esp_netif_ip_info_t *ip_info)
{
    if (s_endpoint_user_override) {
        return;
    }

    const wifi_profile_t *profile = wifi_current_profile();
    if (profile && profile->endpoint && profile->endpoint[0]) {
        strlcpy(s_endpoint, profile->endpoint, sizeof(s_endpoint));
    } else if (profile && profile->endpoint_host_octet != 0 && ip_info) {
        snprintf(s_endpoint,
                 sizeof(s_endpoint),
                 "ws://%u.%u.%u.%u:8080/esp32_ws",
                 esp_ip4_addr1_16(&ip_info->ip),
                 esp_ip4_addr2_16(&ip_info->ip),
                 esp_ip4_addr3_16(&ip_info->ip),
                 (unsigned)profile->endpoint_host_octet);
    } else {
        strlcpy(s_endpoint, ROBOT_MCP_URI, sizeof(s_endpoint));
    }
    ESP_LOGI(TAG,
             "endpoint profile ssid=%s endpoint=%s",
             profile && profile->ssid ? profile->ssid : "<default>",
             s_endpoint[0] ? s_endpoint : "<empty>");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_status = MCP_STATUS_WIFI_CONNECTING;
        app_ui_set_wifi_connected(false);
        app_ui_set_mcp_status(mcp_client_get_status_text());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        int reason = event ? event->reason : -1;
        s_wifi_connected = false;
        s_ws_connected = false;
        s_last_ip_info_valid = false;
        audio_upload_drop_queued_items();
        websocket_request_restart();
        xEventGroupClearBits(s_event_group, MCP_WIFI_CONNECTED_BIT);
        s_status = MCP_STATUS_WIFI_CONNECTING;
        app_ui_set_wifi_connected(false);
        ui_post_event(MCP_UI_EVENT_DISCONNECTED, APP_UI_STATE_OFFLINE, NULL);
        app_ui_set_mcp_status("WIFI RETRY");
        s_wifi_profile_failures++;
        if (WIFI_PROFILE_COUNT > 1 && s_wifi_profile_failures >= WIFI_PROFILE_MAX_FAILURES) {
            ESP_LOGW(TAG,
                     "wifi disconnected reason=%d, switch profile after %u failures",
                     reason,
                     (unsigned)s_wifi_profile_failures);
            s_wifi_profile_failures = 0;
            wifi_select_next_profile();
        } else {
            const wifi_profile_t *profile = wifi_current_profile();
            ESP_LOGW(TAG,
                     "wifi disconnected reason=%d, retry ssid=%s failure=%u/%u",
                     reason,
                     profile && profile->ssid ? profile->ssid : "<empty>",
                     (unsigned)s_wifi_profile_failures,
                     (unsigned)WIFI_PROFILE_MAX_FAILURES);
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event) {
            s_last_ip_info = event->ip_info;
            s_last_ip_info_valid = true;
            endpoint_apply_wifi_profile(&event->ip_info);
        }
        s_wifi_profile_failures = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_event_group, MCP_WIFI_CONNECTED_BIT);
        s_status = MCP_STATUS_WIFI_CONNECTED;
        websocket_request_restart();
        app_ui_set_wifi_connected(true);
        app_ui_set_mcp_status(mcp_client_get_status_text());
        ESP_LOGI(TAG, "wifi connected ip=" IPSTR, IP2STR(&event->ip_info.ip));
        start_sntp_once();
    }
}

static esp_err_t wifi_start(void)
{
    if (s_netif_ready) {
        return ESP_OK;
    }

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_ret, TAG, "nvs_flash_init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create_default failed: %s", esp_err_to_name(event_ret));
        return event_ret;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL), TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL), TAG, "ip handler failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(wifi_apply_profile(s_wifi_profile_index), TAG, "set wifi profile failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set wifi ps failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_netif_ready = true;
    const wifi_profile_t *profile = wifi_current_profile();
    ESP_LOGI(TAG,
             "wifi start ssid=%s profiles=%u",
             profile && profile->ssid ? profile->ssid : "<empty>",
             (unsigned)WIFI_PROFILE_COUNT);
    return ESP_OK;
}

static esp_err_t websocket_create_and_start(void)
{
    if (!s_endpoint[0]) {
        s_status = MCP_STATUS_NOT_CONFIGURED;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ws) {
        return ESP_OK;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = s_endpoint,
        .disable_auto_reconnect = false,
        .task_stack = 4096,
        .buffer_size = 16384,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 3000,
        .ping_interval_sec = 15,
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        s_status = MCP_STATUS_ERROR;
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL), TAG, "ws register failed");
    s_status = MCP_STATUS_CONNECTING;
    app_ui_set_mcp_status(mcp_client_get_status_text());
    log_heap_state("before ws start");
    esp_err_t start_ret = esp_websocket_client_start(s_ws);
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed: %s", esp_err_to_name(start_ret));
        log_heap_state("after ws start failed");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_status = MCP_STATUS_ERROR;
        app_ui_set_mcp_status(mcp_client_get_status_text());
        return start_ret;
    }
    s_last_ws_start_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "websocket start %s", s_endpoint);
    return ESP_OK;
}

static void websocket_restart(void)
{
    if (s_ws) {
        ESP_LOGW(TAG, "websocket restart");
        s_ws_stopping = true;
        esp_websocket_client_stop(s_ws);
        s_ws_stopping = false;
        if (s_ws_recreate_requested) {
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
            s_ws_recreate_requested = false;
        }
    }
    s_ws_connected = false;
    s_ws_restart_requested = false;
    s_status = s_wifi_connected ? MCP_STATUS_CONNECTING : MCP_STATUS_WIFI_CONNECTING;
    app_ui_set_mcp_status(mcp_client_get_status_text());
    if (!s_wifi_connected) {
        return;
    }
    if (!s_ws) {
        esp_err_t err = websocket_create_and_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "websocket recreate failed: %s", esp_err_to_name(err));
            websocket_request_restart();
        }
        return;
    }
    esp_err_t err = esp_websocket_client_start(s_ws);
    s_last_ws_start_tick = xTaskGetTickCount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "websocket restart failed: %s", esp_err_to_name(err));
        websocket_request_restart();
    }
}

static void mcp_service_task(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        s_status = MCP_STATUS_ERROR;
        app_ui_set_mcp_status(mcp_client_get_status_text());
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, MCP_WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & MCP_WIFI_CONNECTED_BIT) {
            if (!s_ws) {
                websocket_create_and_start();
            } else if (s_ws_recreate_requested || (s_ws_connected && s_ws_restart_requested)) {
                websocket_restart();
            } else if (!s_ws_connected) {
                TickType_t now = xTaskGetTickCount();
                if (s_ws_restart_requested || (now - s_last_ws_start_tick) >= pdMS_TO_TICKS(6000)) {
                    websocket_restart();
                }
            }
        }
        if (s_ws_connected) {
            mcp_client_send_telemetry();
            vTaskDelay(pdMS_TO_TICKS(30000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_ws_connected = true;
            s_ws_stopping = false;
            s_ws_restart_requested = false;
            s_status = MCP_STATUS_CONNECTED;
            app_ui_set_mcp_status(mcp_client_get_status_text());
            ui_post_event(MCP_UI_EVENT_CONNECTED, APP_UI_STATE_IDLE, NULL);
            ESP_LOGI(TAG, "websocket connected");
            mcp_client_send_telemetry();
            if (s_cfg_valid) {
                send_config_payload(s_cfg_persona_id,
                                    s_cfg_persona_label,
                                    s_cfg_voice_id,
                                    s_cfg_voice_label,
                                    s_cfg_continuous_chat,
                                    s_cfg_wake_enabled);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s_ws_connected = false;
            audio_upload_drop_queued_items();
            if (!s_ws_stopping) {
                websocket_request_restart();
            }
            s_status = MCP_STATUS_DISCONNECTED;
            app_ui_set_mcp_status(mcp_client_get_status_text());
            ui_post_event(MCP_UI_EVENT_DISCONNECTED, APP_UI_STATE_OFFLINE, NULL);
            ESP_LOGW(TAG, "websocket disconnected");
            break;
        case WEBSOCKET_EVENT_ERROR:
            s_ws_connected = false;
            audio_upload_drop_queued_items();
            if (!s_ws_stopping) {
                websocket_request_restart();
            }
            s_status = MCP_STATUS_ERROR;
            app_ui_set_mcp_status(mcp_client_get_status_text());
            ui_post_event(MCP_UI_EVENT_DISCONNECTED, APP_UI_STATE_OFFLINE, NULL);
            ESP_LOGW(TAG, "websocket error");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (!data || data->data_len <= 0) {
                break;
            }
            if (data->op_code != 0x1 && data->op_code != 0x0) {
                break;
            }
            if (data->op_code == 0x0 && s_rx_len == 0 && data->payload_offset != 0) {
                break;
            }
            if (data->payload_offset == 0) {
                ESP_LOGI(TAG, "ws text incoming payload_len=%d data_len=%d", data->payload_len, data->data_len);
            }
            if (data->payload_offset == 0) {
                s_rx_len = 0;
                s_rx_dropping = false;
            }
            if (s_rx_dropping) {
                if (data->fin) {
                    s_rx_dropping = false;
                    s_rx_len = 0;
                }
                break;
            }
            if (rx_reserve(s_rx_len + (size_t)data->data_len + 1)) {
                memcpy(s_rx_buffer + s_rx_len, data->data_ptr, data->data_len);
                s_rx_len += (size_t)data->data_len;
                s_rx_buffer[s_rx_len] = '\0';
            } else {
                ESP_LOGW(TAG, "ws rx too large, drop payload_len=%d", data->payload_len);
                s_rx_dropping = true;
                s_rx_len = 0;
                break;
            }
            bool complete = data->payload_len > 0
                                ? (data->payload_offset + data->data_len >= data->payload_len)
                                : data->fin;
            if (complete) {
                handle_ws_text(s_rx_buffer);
                s_rx_len = 0;
            }
            break;
        default:
            break;
    }
}

esp_err_t mcp_client_init(void)
{
    s_event_group = xEventGroupCreate();
    s_send_lock = xSemaphoreCreateMutex();
    s_tts_play_queue = xQueueCreateWithCaps(MCP_TTS_PLAY_QUEUE_LEN, sizeof(tts_play_item_t), MCP_QUEUE_CAPS);
    s_audio_upload_queue = xQueueCreateWithCaps(MCP_AUDIO_UPLOAD_QUEUE_LEN, sizeof(audio_upload_item_t), MCP_QUEUE_CAPS);
    s_ui_event_queue = xQueueCreateWithCaps(MCP_UI_EVENT_QUEUE_LEN, sizeof(mcp_ui_event_t), MCP_QUEUE_CAPS);
    if (!s_event_group || !s_send_lock || !s_tts_play_queue || !s_audio_upload_queue || !s_ui_event_queue) {
        log_heap_state("mcp queue create failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret == ESP_OK) {
        esp_err_t endpoint_ret = load_endpoint_from_nvs();
        if (endpoint_ret != ESP_OK) {
            ESP_LOGW(TAG, "endpoint nvs load failed: %s", esp_err_to_name(endpoint_ret));
        }
    } else {
        ESP_LOGW(TAG, "nvs_flash_init for endpoint failed: %s", esp_err_to_name(nvs_ret));
    }
    ESP_RETURN_ON_ERROR(create_spiram_task(tts_play_task, "tts_play", 6144, 5), TAG, "tts task failed");
    ESP_RETURN_ON_ERROR(create_spiram_task(audio_upload_task, "audio_upload", 6144, 5), TAG, "audio upload task failed");
    ESP_RETURN_ON_ERROR(create_spiram_task(ui_event_task, "mcp_ui_events", 3072, 3), TAG, "ui event task failed");
    s_status = s_endpoint[0] ? MCP_STATUS_CONFIGURED : MCP_STATUS_NOT_CONFIGURED;
    log_heap_state("mcp init ready");
    ESP_LOGI(TAG, "status=%s", mcp_client_get_status_text());
    return ESP_OK;
}

esp_err_t mcp_client_set_endpoint(const char *endpoint)
{
    if (!endpoint || endpoint[0] == '\0') {
        s_endpoint[0] = '\0';
        s_endpoint_user_override = true;
        s_status = MCP_STATUS_NOT_CONFIGURED;
        esp_err_t nvs_ret = save_endpoint_to_nvs(NULL);
        if (nvs_ret != ESP_OK) {
            ESP_LOGW(TAG, "endpoint nvs clear failed: %s", esp_err_to_name(nvs_ret));
        }
        ESP_LOGW(TAG, "endpoint cleared");
        return ESP_OK;
    }

    strlcpy(s_endpoint, endpoint, sizeof(s_endpoint));
    s_endpoint_user_override = true;
    s_status = MCP_STATUS_CONFIGURED;
    esp_err_t nvs_ret = save_endpoint_to_nvs(s_endpoint);
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "endpoint nvs save failed: %s", esp_err_to_name(nvs_ret));
    }
    ESP_LOGI(TAG, "endpoint=%s", s_endpoint);
    if (s_ws) {
        websocket_request_recreate();
    }
    return ESP_OK;
}

esp_err_t mcp_client_reset_endpoint_to_default(void)
{
    s_endpoint_user_override = false;
    if (s_last_ip_info_valid) {
        endpoint_apply_wifi_profile(&s_last_ip_info);
    } else {
        strlcpy(s_endpoint, ROBOT_MCP_URI, sizeof(s_endpoint));
    }
    s_status = s_endpoint[0] ? MCP_STATUS_CONFIGURED : MCP_STATUS_NOT_CONFIGURED;

    esp_err_t nvs_ret = save_endpoint_to_nvs(NULL);
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "endpoint nvs default reset failed: %s", esp_err_to_name(nvs_ret));
    }
    ESP_LOGI(TAG, "endpoint reset to default=%s", s_endpoint[0] ? s_endpoint : "<empty>");
    if (s_ws) {
        websocket_request_recreate();
    }
    return nvs_ret;
}

esp_err_t mcp_client_connect(void)
{
    if (!s_endpoint[0]) {
        s_status = MCP_STATUS_NOT_CONFIGURED;
        ESP_LOGW(TAG, "MCP endpoint missing");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_started) {
        s_started = true;
        xTaskCreate(mcp_service_task, "mcp_service", 6144, NULL, 4, NULL);
    }
    return ESP_OK;
}

void mcp_client_disconnect(void)
{
    if (s_ws) {
        s_ws_stopping = true;
        esp_websocket_client_stop(s_ws);
        s_ws_stopping = false;
    }
    s_ws_connected = false;
    audio_upload_drop_queued_items();
    s_status = s_endpoint[0] ? MCP_STATUS_DISCONNECTED : MCP_STATUS_NOT_CONFIGURED;
    ui_post_event(MCP_UI_EVENT_DISCONNECTED, APP_UI_STATE_OFFLINE, NULL);
    ESP_LOGI(TAG, "disconnected");
}

const char *mcp_client_get_endpoint(void)
{
    return s_endpoint;
}

const char *mcp_client_get_status_text(void)
{
    switch (s_status) {
        case MCP_STATUS_CONFIGURED:
            return "MCP CONFIGURED";
        case MCP_STATUS_WIFI_CONNECTING:
            return "WIFI CONNECT";
        case MCP_STATUS_WIFI_CONNECTED:
            return "WIFI OK";
        case MCP_STATUS_CONNECTING:
            return "MCP CONNECTING";
        case MCP_STATUS_CONNECTED:
            return "MCP CONNECTED";
        case MCP_STATUS_DISCONNECTED:
            return "MCP DISCONNECTED";
        case MCP_STATUS_ERROR:
            return "MCP ERROR";
        case MCP_STATUS_NOT_CONFIGURED:
        default:
            return "MCP NOT CONFIGURED";
    }
}

bool mcp_client_is_configured(void)
{
    return s_endpoint[0] != '\0';
}

bool mcp_client_is_wifi_connected(void)
{
    return s_wifi_connected;
}

bool mcp_client_is_connected(void)
{
    return s_ws_connected;
}

bool mcp_client_is_assistant_busy(void)
{
    return s_assistant_busy;
}

void mcp_client_set_sr_enabled(bool enabled)
{
    s_sr_enabled = enabled;
    if (s_ws_connected) {
        mcp_client_send_telemetry();
    }
}

void mcp_client_set_busy_callback(mcp_client_busy_cb_t cb, void *ctx)
{
    s_busy_cb = cb;
    s_busy_ctx = ctx;
}

void mcp_client_set_playback_callback(mcp_client_playback_cb_t cb, void *ctx)
{
    s_playback_cb = cb;
    s_playback_ctx = ctx;
}

void mcp_client_set_device_command_callback(mcp_client_device_command_cb_t cb, void *ctx)
{
    s_device_command_cb = cb;
    s_device_command_ctx = ctx;
}

esp_err_t mcp_client_send_text_request(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char escaped[256];
    char payload[512];
    json_escape(text, escaped, sizeof(escaped));
    snprintf(payload, sizeof(payload), "{\"type\":\"audio_text\",\"device_id\":\"%s\",\"content\":\"%s\"}", ROBOT_DEVICE_ID, escaped);
    ESP_LOGI(TAG, "send text request: %s", text);
    app_ui_set_mcp_status("MCP SEND");
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_THINKING, text);
    esp_err_t err = ws_send_json(payload);
    if (err == ESP_OK) {
        app_ui_note_mcp_activity();
    }
    return err;
}

esp_err_t mcp_client_send_diagnostic_event(const char *name, const char *phase, const char *detail)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char name_escaped[48];
    char phase_escaped[48];
    char *detail_escaped = heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!detail_escaped) {
        detail_escaped = heap_caps_malloc(512, MALLOC_CAP_8BIT);
    }
    char *payload = heap_caps_malloc(768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
        payload = heap_caps_malloc(768, MALLOC_CAP_8BIT);
    }
    if (!detail_escaped || !payload) {
        free(detail_escaped);
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    json_escape(name, name_escaped, sizeof(name_escaped));
    json_escape(phase ? phase : "", phase_escaped, sizeof(phase_escaped));
    json_escape(detail ? detail : "", detail_escaped, 512);
    snprintf(payload,
             768,
             "{\"type\":\"diagnostic_event\",\"device_id\":\"%s\",\"name\":\"%s\",\"phase\":\"%s\",\"detail\":\"%s\"}",
             ROBOT_DEVICE_ID,
             name_escaped,
             phase_escaped,
             detail_escaped);
    ESP_LOGI(TAG, "diagnostic event name=%s phase=%s detail=%s",
             name,
             phase ? phase : "",
             detail ? detail : "");
    esp_err_t ret = ws_send_json(payload);
    free(payload);
    free(detail_escaped);
    return ret;
}

esp_err_t mcp_client_audio_stream_begin(const char *session_id)
{
    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_upload_item_t item = {
        .kind = AUDIO_UPLOAD_ITEM_START,
    };
    strlcpy(item.session_id, session_id, sizeof(item.session_id));
    ESP_LOGI(TAG, "audio stream begin session=%s", session_id);
    strlcpy(s_current_session_id, session_id, sizeof(s_current_session_id));
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_RECORDING, NULL);
    return audio_upload_queue_item(&item,
                                   pdMS_TO_TICKS(MCP_AUDIO_UPLOAD_BEGIN_WAIT_MS),
                                   "audio start")
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t mcp_client_audio_stream_chunk(const char *session_id, const uint8_t *data, size_t len)
{
    if (!session_id || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_upload_item_t item = {
        .kind = AUDIO_UPLOAD_ITEM_CHUNK,
        .len = len,
    };
    strlcpy(item.session_id, session_id, sizeof(item.session_id));
    item.data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!item.data) {
        item.data = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    if (!item.data) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(item.data, data, len);
    return audio_upload_queue_item(&item, 0, "audio chunk") ? ESP_OK : ESP_FAIL;
}

esp_err_t mcp_client_audio_stream_end(const char *session_id, uint32_t duration_ms, const char *reason)
{
    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_upload_item_t item = {
        .kind = AUDIO_UPLOAD_ITEM_END,
        .duration_ms = duration_ms,
    };
    strlcpy(item.session_id, session_id, sizeof(item.session_id));
    strlcpy(item.reason, reason ? reason : "set_release", sizeof(item.reason));
    ESP_LOGI(TAG, "audio stream end session=%s duration=%ums", session_id, (unsigned)duration_ms);
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_UPLOADING, NULL);
    return audio_upload_queue_item(&item,
                                   pdMS_TO_TICKS(MCP_AUDIO_UPLOAD_END_WAIT_MS),
                                   "audio end")
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t send_config_payload(const char *persona_id,
                                     const char *persona_label,
                                     const char *voice_id,
                                     const char *voice_label,
                                     bool continuous_chat,
                                     bool wake_enabled)
{
    char persona_id_escaped[48];
    char persona_label_escaped[64];
    char voice_id_escaped[48];
    char voice_label_escaped[64];
    char payload[512];
    json_escape(persona_id, persona_id_escaped, sizeof(persona_id_escaped));
    json_escape(persona_label, persona_label_escaped, sizeof(persona_label_escaped));
    json_escape(voice_id, voice_id_escaped, sizeof(voice_id_escaped));
    json_escape(voice_label, voice_label_escaped, sizeof(voice_label_escaped));
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"client_config\",\"device_id\":\"%s\",\"persona_id\":\"%s\",\"persona_label\":\"%s\","
             "\"voice_id\":\"%s\",\"voice_label\":\"%s\",\"continuous_chat\":%s,\"wake_enabled\":%s,"
             "\"wake_word\":\"doubao\"}",
             ROBOT_DEVICE_ID,
             persona_id_escaped,
             persona_label_escaped,
             voice_id_escaped,
             voice_label_escaped,
             continuous_chat ? "true" : "false",
             wake_enabled ? "true" : "false");
    ESP_LOGI(TAG, "send client_config persona=%s voice=%s continuous=%d wake=%d",
             persona_id ? persona_id : "",
             voice_id ? voice_id : "",
             continuous_chat,
             wake_enabled);
    return ws_send_json(payload);
}

esp_err_t mcp_client_send_config(const char *persona_id,
                                 const char *persona_label,
                                 const char *voice_id,
                                 const char *voice_label,
                                 bool continuous_chat,
                                 bool wake_enabled)
{
    strlcpy(s_cfg_persona_id, persona_id ? persona_id : "default", sizeof(s_cfg_persona_id));
    strlcpy(s_cfg_persona_label, persona_label ? persona_label : "默认人设", sizeof(s_cfg_persona_label));
    strlcpy(s_cfg_voice_id, voice_id ? voice_id : "default", sizeof(s_cfg_voice_id));
    strlcpy(s_cfg_voice_label, voice_label ? voice_label : "默认音色", sizeof(s_cfg_voice_label));
    s_cfg_continuous_chat = continuous_chat;
    s_cfg_wake_enabled = wake_enabled;
    s_cfg_valid = true;
    if (!s_ws_connected) {
        ESP_LOGI(TAG, "cache client_config until websocket connected");
        return ESP_OK;
    }
    return send_config_payload(s_cfg_persona_id,
                               s_cfg_persona_label,
                               s_cfg_voice_id,
                               s_cfg_voice_label,
                               s_cfg_continuous_chat,
                               s_cfg_wake_enabled);
}

esp_err_t mcp_client_send_telemetry(void)
{
    if (!s_ws_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char payload[256];
    int rssi = -127;
    if (s_wifi_connected) {
        esp_wifi_sta_get_rssi(&rssi);
    }
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"telemetry\",\"device_id\":\"%s\",\"free_heap\":%u,\"wifi_rssi\":%d,\"uptime_ms\":%u,\"ws_connected\":true,\"sr_enabled\":%s}",
             ROBOT_DEVICE_ID,
             (unsigned)esp_get_free_heap_size(),
             rssi,
             (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
             s_sr_enabled ? "true" : "false");
    esp_err_t err = ws_send_json(payload);
    if (err == ESP_OK) {
        app_ui_set_mcp_connected(true);
        app_ui_note_mcp_activity();
    }
    return err;
}
