#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_buttons.h"
#include "app_ui.h"
#include "audio_player.h"
#include "esp_log.h"
#include "mcp_client.h"
#include "mic_diag.h"
#include "sdkconfig.h"

#define VOICE_LOOP_GAP_MS 500
#define CAPTURE_CHUNK_BYTES 4096
#define DEBUG_REC_MIN_MS 300
#define DEBUG_REC_MAX_MS 10000

typedef enum {
    VOICE_CMD_PLAY_ONCE,
    VOICE_CMD_LOOP,
    VOICE_CMD_STOP,
    VOICE_CMD_VOL_SET,
    VOICE_CMD_VOL_UP,
    VOICE_CMD_VOL_DOWN,
    VOICE_CMD_MIC_ON,
    VOICE_CMD_MIC_OFF,
    VOICE_CMD_MCP_CONNECT,
    VOICE_CMD_MCP_DISCONNECT,
    VOICE_CMD_SET_PRESS,
    VOICE_CMD_SET_RELEASE,
} voice_cmd_type_t;

typedef struct {
    voice_cmd_type_t type;
    int value;
} voice_cmd_t;

static const char *TAG = "ROBOT_VOICE";
static QueueHandle_t s_cmd_queue;
static uint8_t s_capture_chunk[CAPTURE_CHUNK_BYTES];
static size_t s_capture_chunk_len;
static char s_capture_session_id[32];
static uint32_t s_capture_seq;

static void print_help(void)
{
    ESP_LOGI(TAG, "commands: ASK <text>, REC <ms>, PLAY/XIAOLE, LOOP, STOP, MIC ON, MIC OFF, VOL 0-100, VOL+, VOL-, MCP URL <url>, MCP CONNECT, HELP");
}

static void send_cmd(voice_cmd_type_t type, int value)
{
    if (!s_cmd_queue) {
        return;
    }
    voice_cmd_t cmd = {
        .type = type,
        .value = value,
    };
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropped type=%d", type);
    }
}

static void send_cmd_nonblocking(voice_cmd_type_t type, int value)
{
    if (!s_cmd_queue) {
        return;
    }
    voice_cmd_t cmd = {
        .type = type,
        .value = value,
    };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropped ui type=%d", type);
    }
}

static void send_cmd_from_isr_safe(voice_cmd_type_t type, int value)
{
    send_cmd(type, value);
}

static void on_ui_action(app_ui_action_t action, void *ctx)
{
    (void)ctx;
    switch (action) {
        case APP_UI_ACTION_TALK_PRESS:
            send_cmd_nonblocking(VOICE_CMD_SET_PRESS, 0);
            break;
        case APP_UI_ACTION_TALK_RELEASE:
            send_cmd_nonblocking(VOICE_CMD_SET_RELEASE, 0);
            break;
        case APP_UI_ACTION_MCP_CONNECT:
            send_cmd_nonblocking(VOICE_CMD_MCP_CONNECT, 0);
            break;
        case APP_UI_ACTION_MCP_DISCONNECT:
            send_cmd_nonblocking(VOICE_CMD_MCP_DISCONNECT, 0);
            break;
        case APP_UI_ACTION_MCP_RECONNECT:
            send_cmd_nonblocking(VOICE_CMD_MCP_DISCONNECT, 0);
            send_cmd_nonblocking(VOICE_CMD_MCP_CONNECT, 0);
            break;
        case APP_UI_ACTION_VOL_UP:
            send_cmd_nonblocking(VOICE_CMD_VOL_UP, 0);
            break;
        case APP_UI_ACTION_VOL_DOWN:
            send_cmd_nonblocking(VOICE_CMD_VOL_DOWN, 0);
            break;
        case APP_UI_ACTION_PLAY_TEST:
            send_cmd_nonblocking(VOICE_CMD_PLAY_ONCE, 0);
            break;
        case APP_UI_ACTION_BLUETOOTH_TOGGLE:
#if CONFIG_BT_ENABLED
            app_ui_set_bluetooth_enabled(true);
            app_ui_set_voice_state("BT TODO");
#else
            app_ui_set_bluetooth_enabled(false);
            app_ui_set_voice_state("BT DISABLED");
#endif
            break;
    }
}

static void on_mic_level(int peak, int avg_abs)
{
    app_ui_set_mic_level(peak, avg_abs);
}

static void flush_capture_chunk(void)
{
    if (s_capture_chunk_len == 0) {
        return;
    }
    esp_err_t err = mcp_client_audio_stream_chunk(s_capture_session_id, s_capture_chunk, s_capture_chunk_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio chunk send failed: %s", esp_err_to_name(err));
        app_ui_set_mcp_status("MCP SEND FAIL");
    }
    s_capture_chunk_len = 0;
}

static void on_capture_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if (!data || len <= 0 || !mcp_client_is_connected()) {
        return;
    }

    int offset = 0;
    while (offset < len) {
        size_t room = sizeof(s_capture_chunk) - s_capture_chunk_len;
        size_t copy_len = (size_t)(len - offset);
        if (copy_len > room) {
            copy_len = room;
        }
        memcpy(s_capture_chunk + s_capture_chunk_len, data + offset, copy_len);
        s_capture_chunk_len += copy_len;
        offset += (int)copy_len;
        if (s_capture_chunk_len == sizeof(s_capture_chunk)) {
            flush_capture_chunk();
        }
    }
}

static void start_set_capture(void)
{
    if (!mcp_client_is_connected()) {
        ESP_LOGW(TAG, "SET ignored: MCP not connected");
        app_ui_set_mcp_status(mcp_client_get_status_text());
        app_ui_set_assistant_state(APP_UI_STATE_OFFLINE);
        return;
    }
    if (mic_diag_is_capturing()) {
        return;
    }

    snprintf(s_capture_session_id,
             sizeof(s_capture_session_id),
             "esp32-%lu-%lu",
             (unsigned long)xTaskGetTickCount(),
             (unsigned long)++s_capture_seq);
    s_capture_chunk_len = 0;

    esp_err_t err = mcp_client_audio_stream_begin(s_capture_session_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio stream begin failed: %s", esp_err_to_name(err));
        app_ui_set_mcp_status("MCP SEND FAIL");
        return;
    }

    err = mic_diag_capture_start(on_capture_audio, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mic capture start failed: %s", esp_err_to_name(err));
        mcp_client_audio_stream_end(s_capture_session_id, 0, "mic_start_failed");
        app_ui_set_mic_state("MIC ERROR");
        return;
    }

    app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
    app_ui_set_mic_state("REC SET");
}

static void stop_set_capture(void)
{
    if (!mic_diag_is_capturing()) {
        return;
    }

    uint32_t duration_ms = mic_diag_capture_stop();
    flush_capture_chunk();
    mcp_client_audio_stream_end(s_capture_session_id, duration_ms, "set_release");
    app_ui_set_assistant_state(APP_UI_STATE_UPLOADING);
    app_ui_set_mic_state("MIC READY");
}

static void on_button_event(app_button_event_t event, void *ctx)
{
    (void)ctx;
    switch (event) {
        case APP_BUTTON_SET_PRESS:
            send_cmd_from_isr_safe(VOICE_CMD_SET_PRESS, 0);
            break;
        case APP_BUTTON_SET_RELEASE:
            send_cmd_from_isr_safe(VOICE_CMD_SET_RELEASE, 0);
            break;
        case APP_BUTTON_VOL_UP:
            send_cmd_from_isr_safe(VOICE_CMD_VOL_UP, 0);
            break;
        case APP_BUTTON_VOL_DOWN:
            send_cmd_from_isr_safe(VOICE_CMD_VOL_DOWN, 0);
            break;
        case APP_BUTTON_PLAY:
            send_cmd_from_isr_safe(VOICE_CMD_PLAY_ONCE, 0);
            break;
        case APP_BUTTON_MODE:
            app_ui_prev_page();
            break;
        case APP_BUTTON_REC:
            app_ui_next_page();
            break;
    }
}

static void playback_task(void *arg)
{
    bool loop_enabled = false;
    voice_cmd_t cmd = {0};

    ESP_LOGI(TAG, "idle. Hold SET to record and send to MCP.");

    while (true) {
        if (loop_enabled) {
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) {
                cmd.type = VOICE_CMD_PLAY_ONCE;
            }
        } else {
            xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);
        }

        switch (cmd.type) {
            case VOICE_CMD_PLAY_ONCE:
                app_ui_set_assistant_state(APP_UI_STATE_PLAYING);
                audio_player_play_xiaole();
                app_ui_set_assistant_state(APP_UI_STATE_IDLE);
                if (loop_enabled) {
                    vTaskDelay(pdMS_TO_TICKS(VOICE_LOOP_GAP_MS));
                }
                break;
            case VOICE_CMD_LOOP:
                loop_enabled = true;
                ESP_LOGI(TAG, "loop enabled");
                app_ui_set_voice_state("VOICE LOOP");
                break;
            case VOICE_CMD_STOP:
                loop_enabled = false;
                ESP_LOGI(TAG, "loop stopped");
                app_ui_set_voice_state("VOICE STOP");
                break;
            case VOICE_CMD_VOL_SET:
                audio_player_set_volume(cmd.value);
                app_ui_set_volume(audio_player_get_volume());
                break;
            case VOICE_CMD_VOL_UP:
                audio_player_adjust_volume(10);
                app_ui_set_volume(audio_player_get_volume());
                break;
            case VOICE_CMD_VOL_DOWN:
                audio_player_adjust_volume(-10);
                app_ui_set_volume(audio_player_get_volume());
                break;
            case VOICE_CMD_MIC_ON:
                mic_diag_start();
                app_ui_set_mic_state("MIC ON");
                break;
            case VOICE_CMD_MIC_OFF:
                mic_diag_stop();
                app_ui_set_mic_state("MIC OFF");
                break;
            case VOICE_CMD_MCP_CONNECT:
                mcp_client_connect();
                app_ui_set_mcp_status(mcp_client_get_status_text());
                break;
            case VOICE_CMD_MCP_DISCONNECT:
                mcp_client_disconnect();
                app_ui_set_mcp_status(mcp_client_get_status_text());
                break;
            case VOICE_CMD_SET_PRESS:
                start_set_capture();
                break;
            case VOICE_CMD_SET_RELEASE:
                stop_set_capture();
                break;
        }
    }
}

static void uppercase_line(char *line)
{
    for (char *p = line; *p; ++p) {
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
        *p = (char)toupper((unsigned char)*p);
    }
}

static void command_task(void *arg)
{
    char raw_line[160];
    char line[160];
    print_help();

    while (true) {
        if (!fgets(raw_line, sizeof(raw_line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        strlcpy(line, raw_line, sizeof(line));
        uppercase_line(line);
        for (char *p = raw_line; *p; ++p) {
            if (*p == '\r' || *p == '\n') {
                *p = '\0';
                break;
            }
        }

        if (strcmp(line, "PLAY") == 0 || strcmp(line, "XIAOLE") == 0) {
            send_cmd(VOICE_CMD_PLAY_ONCE, 0);
        } else if (strcmp(line, "LOOP") == 0) {
            send_cmd(VOICE_CMD_LOOP, 0);
        } else if (strcmp(line, "STOP") == 0) {
            send_cmd(VOICE_CMD_STOP, 0);
        } else if (strcmp(line, "MIC ON") == 0 || strcmp(line, "MIC") == 0) {
            send_cmd(VOICE_CMD_MIC_ON, 0);
        } else if (strcmp(line, "MIC OFF") == 0) {
            send_cmd(VOICE_CMD_MIC_OFF, 0);
        } else if (strcmp(line, "VOL+") == 0 || strcmp(line, "V+") == 0) {
            send_cmd(VOICE_CMD_VOL_UP, 0);
        } else if (strcmp(line, "VOL-") == 0 || strcmp(line, "V-") == 0) {
            send_cmd(VOICE_CMD_VOL_DOWN, 0);
        } else if (strncmp(line, "VOL ", 4) == 0 || strncmp(line, "V ", 2) == 0) {
            char *value_text = strchr(line, ' ');
            send_cmd(VOICE_CMD_VOL_SET, value_text ? atoi(value_text + 1) : audio_player_get_volume());
        } else if (strncmp(line, "MCP URL ", 8) == 0) {
            const char *endpoint = raw_line + 8;
            mcp_client_set_endpoint(endpoint);
            app_ui_set_mcp_status(mcp_client_get_status_text());
        } else if (strcmp(line, "MCP CONNECT") == 0 || strcmp(line, "MCP") == 0) {
            send_cmd(VOICE_CMD_MCP_CONNECT, 0);
        } else if (strcmp(line, "MCP DISCONNECT") == 0) {
            send_cmd(VOICE_CMD_MCP_DISCONNECT, 0);
        } else if (strncmp(line, "ASK ", 4) == 0) {
            const char *text = raw_line + 4;
            if (!mcp_client_is_connected()) {
                ESP_LOGW(TAG, "ASK ignored: MCP not connected");
                app_ui_set_mcp_status(mcp_client_get_status_text());
            } else {
                ESP_LOGI(TAG, "ASK debug text: %s", text);
                app_ui_set_recent_text(text);
                app_ui_set_assistant_state(APP_UI_STATE_THINKING);
                mcp_client_send_text_request(text);
            }
        } else if (strncmp(line, "REC ", 4) == 0) {
            int duration_ms = atoi(line + 4);
            if (duration_ms < DEBUG_REC_MIN_MS) {
                duration_ms = DEBUG_REC_MIN_MS;
            } else if (duration_ms > DEBUG_REC_MAX_MS) {
                duration_ms = DEBUG_REC_MAX_MS;
            }
            ESP_LOGI(TAG, "REC debug capture %dms", duration_ms);
            send_cmd(VOICE_CMD_SET_PRESS, 0);
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
            send_cmd(VOICE_CMD_SET_RELEASE, 0);
        } else if (strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0) {
            print_help();
        } else if (line[0] != '\0') {
            ESP_LOGW(TAG, "unknown command: %s", line);
            print_help();
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("AUDIO_PLAYER", ESP_LOG_INFO);
    esp_log_level_set("MIC_DIAG", ESP_LOG_INFO);
    esp_log_level_set("APP_UI", ESP_LOG_INFO);
    esp_log_level_set("MCP_CLIENT", ESP_LOG_INFO);

    printf("\n==== ESP-IDF ADF ROBOT VOICE ====\n");
    printf("SET key push-to-talk + MCP websocket + LCD status.\n");

    if (audio_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio player init failed");
        return;
    }

    if (app_ui_init() != ESP_OK) {
        ESP_LOGW(TAG, "ui init failed; continue without lcd");
    }
    app_ui_set_action_callback(on_ui_action, NULL);
    app_ui_set_volume(audio_player_get_volume());
    app_ui_set_voice_state("VOICE READY");
#if CONFIG_BT_ENABLED
    app_ui_set_bluetooth_available(true);
    app_ui_set_bluetooth_enabled(false);
#else
    app_ui_set_bluetooth_available(false);
#endif

    if (mic_diag_init(on_mic_level) != ESP_OK) {
        ESP_LOGW(TAG, "mic diag init failed; MIC commands unavailable");
    }
    app_ui_set_mic_state("MIC READY");

    mcp_client_init();
    app_ui_set_mcp_status(mcp_client_get_status_text());

    s_cmd_queue = xQueueCreate(16, sizeof(voice_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "command queue create failed");
        return;
    }

    xTaskCreate(playback_task, "voice_playback", 4096, NULL, 5, NULL);
    xTaskCreate(command_task, "voice_command", 4096, NULL, 4, NULL);

    if (app_buttons_init(on_button_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button init failed");
    }

    send_cmd(VOICE_CMD_MCP_CONNECT, 0);
}
