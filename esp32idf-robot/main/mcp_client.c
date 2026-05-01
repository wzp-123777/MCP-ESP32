#include "mcp_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "mbedtls/base64.h"
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

typedef struct {
    uint8_t *wav;
    size_t wav_len;
} tts_play_item_t;

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
static char s_endpoint[160] = ROBOT_MCP_URI;
static mcp_status_t s_status = MCP_STATUS_NOT_CONFIGURED;
static EventGroupHandle_t s_event_group;
static SemaphoreHandle_t s_send_lock;
static QueueHandle_t s_tts_play_queue;
static QueueHandle_t s_ui_event_queue;
static esp_websocket_client_handle_t s_ws;
static bool s_netif_ready;
static bool s_wifi_connected;
static bool s_ws_connected;
static bool s_started;
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
static char s_current_session_id[32];
static app_ui_assistant_state_t s_last_ui_state = APP_UI_STATE_IDLE;
static TickType_t s_last_ui_state_tick;

#define MCP_WIFI_CONNECTED_BIT BIT0
#define MCP_SEND_TIMEOUT pdMS_TO_TICKS(3000)
#define MCP_MAX_RX_MESSAGE (256 * 1024)
#define MCP_MAX_TTS_BYTES (384 * 1024)
#define MCP_MIN_TTS_BYTES 1024
#define MCP_TTS_PLAY_QUEUE_LEN 4
#define MCP_UI_EVENT_QUEUE_LEN 8

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void websocket_restart(void);
static esp_err_t websocket_create_and_start(void);
static void websocket_request_restart(void);
static void websocket_request_recreate(void);
static void tts_play_task(void *arg);
static void ui_event_task(void *arg);
static const char *json_get_string(const char *json, const char *key, char *out, size_t out_size);

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started");
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
        tts_reset();
        return;
    }
    if (!s_tts_buf || s_tts_len <= 44) {
        tts_reset();
        return;
    }
    if (s_tts_len < MCP_MIN_TTS_BYTES) {
        ESP_LOGW(TAG, "tts too small, dropped len=%u", (unsigned)s_tts_len);
        tts_reset();
        return;
    }
    tts_play_item_t item = {
        .wav = s_tts_buf,
        .wav_len = s_tts_len,
    };
    s_tts_buf = NULL;
    s_tts_len = 0;
    s_tts_cap = 0;
    s_tts_b64_pending_len = 0;
    s_tts_chunk_count = 0;
    s_tts_b64_chars = 0;

    if (!s_tts_play_queue || xQueueSend(s_tts_play_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tts play queue full, dropped len=%u", (unsigned)item.wav_len);
        heap_caps_free(item.wav);
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_IDLE, NULL);
        return;
    }
    ESP_LOGI(TAG, "tts queued len=%u", (unsigned)item.wav_len);
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
        if (!item.wav || item.wav_len == 0) {
            continue;
        }
        ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_PLAYING, NULL);
        esp_err_t ret = audio_player_play_wav(item.wav, item.wav_len, "tts");
        ESP_LOGI(TAG, "tts play finished len=%u ret=%s", (unsigned)item.wav_len, esp_err_to_name(ret));
        heap_caps_free(item.wav);
        item.wav = NULL;
        item.wav_len = 0;
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
                s_last_ui_state = APP_UI_STATE_IDLE;
                s_last_ui_state_tick = xTaskGetTickCount();
                break;
            case MCP_UI_EVENT_DISCONNECTED:
                app_ui_set_mcp_connected(false);
                s_last_ui_state = APP_UI_STATE_OFFLINE;
                s_last_ui_state_tick = xTaskGetTickCount();
                break;
            case MCP_UI_EVENT_STATUS:
                app_ui_note_mcp_activity();
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

    if (strcmp(type, "assistant_error") == 0 || strcmp(type, "tts_error") == 0 || strcmp(type, "vision_error") == 0) {
        json_get_string(message, "error", text, sizeof(text));
        ESP_LOGW(TAG, "%s: %s", type, text);
        if (strcmp(type, "tts_error") == 0) {
            tts_reset();
        }
        ui_post_event(MCP_UI_EVENT_ERROR, APP_UI_STATE_ERROR, text);
        return;
    }

    if (strcmp(type, "tts_segment_start") == 0) {
        json_get_string(message, "text", text, sizeof(text));
        ESP_LOGI(TAG, "tts segment: %s", text);
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
        tts_finish();
        return;
    }

    if (strcmp(type, "device_command") == 0) {
        ESP_LOGI(TAG, "device_command: %s", message);
        return;
    }

    ESP_LOGI(TAG, "unhandled ws: %s", message);
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
        s_wifi_connected = false;
        s_ws_connected = false;
        websocket_request_restart();
        xEventGroupClearBits(s_event_group, MCP_WIFI_CONNECTED_BIT);
        s_status = MCP_STATUS_WIFI_CONNECTING;
        app_ui_set_wifi_connected(false);
        ui_post_event(MCP_UI_EVENT_DISCONNECTED, APP_UI_STATE_OFFLINE, NULL);
        app_ui_set_mcp_status("WIFI RETRY");
        ESP_LOGW(TAG, "wifi disconnected reason=%d, reconnecting", event ? event->reason : -1);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        xEventGroupSetBits(s_event_group, MCP_WIFI_CONNECTED_BIT);
        s_status = MCP_STATUS_WIFI_CONNECTED;
        websocket_request_restart();
        app_ui_set_wifi_connected(true);
        app_ui_set_mcp_status(mcp_client_get_status_text());
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
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

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ROBOT_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, ROBOT_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set wifi ps failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_netif_ready = true;
    ESP_LOGI(TAG, "wifi start ssid=%s", ROBOT_WIFI_SSID);
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
        .task_stack = 6144,
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
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_ws), TAG, "ws start failed");
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
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s_ws_connected = false;
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
    s_tts_play_queue = xQueueCreate(MCP_TTS_PLAY_QUEUE_LEN, sizeof(tts_play_item_t));
    s_ui_event_queue = xQueueCreate(MCP_UI_EVENT_QUEUE_LEN, sizeof(mcp_ui_event_t));
    if (!s_event_group || !s_send_lock || !s_tts_play_queue || !s_ui_event_queue) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(tts_play_task, "tts_play", 6144, NULL, 5, NULL);
    xTaskCreate(ui_event_task, "mcp_ui_events", 3072, NULL, 3, NULL);
    s_status = s_endpoint[0] ? MCP_STATUS_CONFIGURED : MCP_STATUS_NOT_CONFIGURED;
    ESP_LOGI(TAG, "status=%s", mcp_client_get_status_text());
    return ESP_OK;
}

esp_err_t mcp_client_set_endpoint(const char *endpoint)
{
    if (!endpoint || endpoint[0] == '\0') {
        s_endpoint[0] = '\0';
        s_status = MCP_STATUS_NOT_CONFIGURED;
        ESP_LOGW(TAG, "endpoint cleared");
        return ESP_OK;
    }

    strlcpy(s_endpoint, endpoint, sizeof(s_endpoint));
    s_status = MCP_STATUS_CONFIGURED;
    ESP_LOGI(TAG, "endpoint=%s", s_endpoint);
    if (s_ws) {
        websocket_request_recreate();
    }
    return ESP_OK;
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

esp_err_t mcp_client_send_text_request(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char escaped[256];
    char payload[384];
    json_escape(text, escaped, sizeof(escaped));
    snprintf(payload, sizeof(payload), "{\"type\":\"audio_text\",\"device_id\":\"%s\",\"content\":\"%s\"}", ROBOT_DEVICE_ID, escaped);
    ESP_LOGI(TAG, "send text request: %s", text);
    app_ui_set_mcp_status("MCP SEND");
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_THINKING, text);
    return ws_send_json(payload);
}

esp_err_t mcp_client_audio_stream_begin(const char *session_id)
{
    char payload[256];
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"audio_stream_start\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"sample_rate\":%d,\"sample_bits\":%d,\"channels\":%d,\"encoding\":\"pcm_s16le\"}",
             ROBOT_DEVICE_ID,
             session_id,
             ROBOT_AUDIO_SAMPLE_RATE,
             ROBOT_AUDIO_BITS,
             ROBOT_AUDIO_CHANNELS);
    ESP_LOGI(TAG, "audio stream begin session=%s", session_id);
    strlcpy(s_current_session_id, session_id ? session_id : "", sizeof(s_current_session_id));
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_RECORDING, NULL);
    return ws_send_json(payload);
}

esp_err_t mcp_client_audio_stream_chunk(const char *session_id, const uint8_t *data, size_t len)
{
    if (!session_id || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t b64_cap = ((len + 2) / 3) * 4 + 1;
    char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_8BIT);
    if (!b64) {
        return ESP_ERR_NO_MEM;
    }
    size_t b64_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len, data, len);
    if (rc != 0) {
        free(b64);
        return ESP_FAIL;
    }
    b64[b64_len] = '\0';

    size_t payload_cap = b64_len + 160;
    char *payload = heap_caps_malloc(payload_cap, MALLOC_CAP_8BIT);
    if (!payload) {
        free(b64);
        return ESP_ERR_NO_MEM;
    }
    snprintf(payload,
             payload_cap,
             "{\"type\":\"audio_stream_chunk\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"audio_b64\":\"%s\"}",
             ROBOT_DEVICE_ID,
             session_id,
             b64);
    esp_err_t err = ws_send_json(payload);
    free(payload);
    free(b64);
    return err;
}

esp_err_t mcp_client_audio_stream_end(const char *session_id, uint32_t duration_ms, const char *reason)
{
    char payload[256];
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"audio_stream_end\",\"device_id\":\"%s\",\"session_id\":\"%s\",\"duration_ms\":%u,\"reason\":\"%s\"}",
             ROBOT_DEVICE_ID,
             session_id,
             (unsigned)duration_ms,
             reason ? reason : "set_release");
    ESP_LOGI(TAG, "audio stream end session=%s duration=%ums", session_id, (unsigned)duration_ms);
    ui_post_event(MCP_UI_EVENT_STATUS, APP_UI_STATE_UPLOADING, NULL);
    return ws_send_json(payload);
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
             "{\"type\":\"telemetry\",\"device_id\":\"%s\",\"free_heap\":%u,\"wifi_rssi\":%d,\"uptime_ms\":%u,\"ws_connected\":true,\"sr_enabled\":false}",
             ROBOT_DEVICE_ID,
             (unsigned)esp_get_free_heap_size(),
             rssi,
             (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    return ws_send_json(payload);
}
