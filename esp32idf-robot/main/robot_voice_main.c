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
#include "afe_full_duplex_plan.h"
#include "esp_log.h"
#include "mcp_client.h"
#include "mic_diag.h"
#include "sdkconfig.h"

#define VOICE_LOOP_GAP_MS 500
#define CAPTURE_CHUNK_BYTES 4096
#define DEBUG_REC_MIN_MS 300
#define DEBUG_REC_MAX_MS 10000
#define CONT_VAD_START_AVG 520
#define CONT_VAD_START_PEAK 2600
#define CONT_VAD_STOP_AVG 260
#define CONT_VAD_STOP_PEAK 1400
#define CONT_VAD_START_HITS 2
#define CONT_VAD_SILENCE_HITS 4
#define CONT_VAD_TAIL_MS 950
#define CONT_VAD_MIN_SPEECH_MS 320
#define CONT_VAD_MAX_SPEECH_MS 12000
#define CONT_REARM_DELAY_MS 900
#define CONT_VAD_NOISE_FLOOR_INIT 140
#define CONT_VAD_NOISE_FLOOR_MIN 60
#define CONT_VAD_NOISE_FLOOR_MAX 460
#define CONT_VAD_START_MARGIN 380
#define CONT_VAD_STOP_MARGIN 170
#define CONT_VAD_LOG_INTERVAL_MS 1000

typedef enum {
    CAPTURE_MODE_NONE = 0,
    CAPTURE_MODE_PTT,
    CAPTURE_MODE_CONTINUOUS,
} capture_mode_t;

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
    VOICE_CMD_CHAT_TOGGLE,
    VOICE_CMD_WAKE_TOGGLE,
    VOICE_CMD_PERSONA_NEXT,
    VOICE_CMD_VOICE_NEXT,
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
static capture_mode_t s_capture_mode;
static bool s_continuous_chat;
static bool s_continuous_speaking;
static bool s_wake_enabled;
static bool s_audio_busy;
static TickType_t s_cont_speech_start_tick;
static TickType_t s_cont_last_voice_tick;
static TickType_t s_cont_rearm_tick;
static TickType_t s_cont_vad_log_tick;
static int s_cont_start_hits;
static int s_cont_silence_hits;
static int s_cont_noise_floor = CONT_VAD_NOISE_FLOOR_INIT;

typedef struct {
    const char *id;
    const char *label;
} config_option_t;

static const config_option_t s_personas[] = {
    {"default", "默认人设"},
    {"sweet", "甜妹人设"},
    {"serious", "认真助手"},
};

static const config_option_t s_voice_profiles[] = {
    {"default", "默认音色"},
    {"female_soft", "柔和女声"},
    {"female_bright", "明亮女声"},
};

static size_t s_persona_index;
static size_t s_voice_index;

static void print_help(void)
{
    ESP_LOGI(TAG, "commands: ASK <text>, REC <ms>, CHAT, WAKE, PERSONA, VOICE, PLAY/XIAOLE, LOOP, STOP, MIC ON, MIC OFF, VOL 0-100, VOL+, VOL-, MCP URL <url>, MCP CONNECT, HELP");
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
            app_ui_set_bluetooth_enabled(false);
            app_ui_set_voice_state("BT DISABLED");
            break;
        case APP_UI_ACTION_CHAT_TOGGLE:
            send_cmd_nonblocking(VOICE_CMD_CHAT_TOGGLE, 0);
            break;
        case APP_UI_ACTION_WAKE_TOGGLE:
            send_cmd_nonblocking(VOICE_CMD_WAKE_TOGGLE, 0);
            break;
        case APP_UI_ACTION_PERSONA_NEXT:
            send_cmd_nonblocking(VOICE_CMD_PERSONA_NEXT, 0);
            break;
        case APP_UI_ACTION_VOICE_NEXT:
            send_cmd_nonblocking(VOICE_CMD_VOICE_NEXT, 0);
            break;
    }
}

static void on_mic_level(int peak, int avg_abs)
{
    app_ui_set_mic_level(peak, avg_abs);
}

static void send_runtime_config(void)
{
    const config_option_t *persona = &s_personas[s_persona_index];
    const config_option_t *voice = &s_voice_profiles[s_voice_index];
    app_ui_set_chat_continuous(s_continuous_chat);
    app_ui_set_wake_enabled(s_wake_enabled);
    app_ui_set_persona(persona->label);
    app_ui_set_voice_profile(voice->label);
    esp_err_t err = mcp_client_send_config(persona->id,
                                           persona->label,
                                           voice->id,
                                           voice->label,
                                           s_continuous_chat,
                                           s_wake_enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send config failed: %s", esp_err_to_name(err));
    }
}

static void make_capture_session_id(void)
{
    snprintf(s_capture_session_id,
             sizeof(s_capture_session_id),
             "esp32-%lu-%lu",
             (unsigned long)xTaskGetTickCount(),
             (unsigned long)++s_capture_seq);
}

static int abs16_sample(int16_t value)
{
    return value < 0 ? -value : value;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static void analyze_pcm_level(const uint8_t *data, int len, int *peak, int *avg_abs)
{
    int sample_count = len / (int)sizeof(int16_t);
    int max_peak = 0;
    int64_t sum_abs = 0;
    const int16_t *samples = (const int16_t *)data;
    for (int i = 0; i < sample_count; ++i) {
        int level = abs16_sample(samples[i]);
        if (level > max_peak) {
            max_peak = level;
        }
        sum_abs += level;
    }
    if (peak) {
        *peak = max_peak;
    }
    if (avg_abs) {
        *avg_abs = sample_count > 0 ? (int)(sum_abs / sample_count) : 0;
    }
}

static int cont_vad_start_threshold(void)
{
    return max_int(CONT_VAD_START_AVG, s_cont_noise_floor + CONT_VAD_START_MARGIN);
}

static int cont_vad_stop_threshold(void)
{
    return max_int(CONT_VAD_STOP_AVG, s_cont_noise_floor + CONT_VAD_STOP_MARGIN);
}

static void cont_vad_reset_runtime(void)
{
    s_cont_start_hits = 0;
    s_cont_silence_hits = 0;
    s_cont_vad_log_tick = 0;
}

static void cont_vad_reset_noise_floor(void)
{
    s_cont_noise_floor = CONT_VAD_NOISE_FLOOR_INIT;
}

static void cont_vad_update_noise_floor(int avg_abs)
{
    int start_threshold = cont_vad_start_threshold();
    if (avg_abs >= start_threshold) {
        return;
    }

    int sample = clamp_int(avg_abs, CONT_VAD_NOISE_FLOOR_MIN, CONT_VAD_NOISE_FLOOR_MAX);
    s_cont_noise_floor = ((s_cont_noise_floor * 15) + sample) / 16;
    s_cont_noise_floor = clamp_int(s_cont_noise_floor,
                                   CONT_VAD_NOISE_FLOOR_MIN,
                                   CONT_VAD_NOISE_FLOOR_MAX);
}

static void cont_vad_log_sample(TickType_t now, int avg_abs, int peak, uint32_t silence_ms)
{
    if ((uint32_t)((now - s_cont_vad_log_tick) * portTICK_PERIOD_MS) < CONT_VAD_LOG_INTERVAL_MS) {
        return;
    }
    s_cont_vad_log_tick = now;
    ESP_LOGI(TAG,
             "cont vad avg=%d peak=%d noise=%d start=%d stop=%d speaking=%d silent_hits=%d silence=%ums",
             avg_abs,
             peak,
             s_cont_noise_floor,
             cont_vad_start_threshold(),
             cont_vad_stop_threshold(),
             s_continuous_speaking,
             s_cont_silence_hits,
             (unsigned)silence_ms);
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

static void drop_capture_chunk(void)
{
    s_capture_chunk_len = 0;
}

static void append_capture_audio(const uint8_t *data, int len)
{
    if (!data || len <= 0 || !mcp_client_is_connected() || s_capture_session_id[0] == '\0') {
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

static void finish_continuous_utterance(const char *reason)
{
    if (!s_continuous_speaking) {
        return;
    }
    bool abort_upload = reason && strcmp(reason, "manual_stop") == 0;
    if (abort_upload) {
        drop_capture_chunk();
    } else {
        flush_capture_chunk();
    }
    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = (uint32_t)((now - s_cont_speech_start_tick) * portTICK_PERIOD_MS);
    if (s_capture_session_id[0] != '\0') {
        mcp_client_audio_stream_end(s_capture_session_id, duration_ms, reason ? reason : "vad_silence");
    }
    ESP_LOGI(TAG, "continuous utterance end reason=%s duration=%ums", reason ? reason : "vad_silence", (unsigned)duration_ms);
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;
    s_continuous_speaking = false;
    cont_vad_reset_runtime();
    s_audio_busy = !abort_upload;
    s_cont_rearm_tick = now + pdMS_TO_TICKS(CONT_REARM_DELAY_MS);
    app_ui_set_assistant_state(abort_upload ? APP_UI_STATE_IDLE : APP_UI_STATE_UPLOADING);
    app_ui_set_mic_state("MIC READY");
}

static void on_capture_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if (s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        if (!data || len <= 0) {
            return;
        }
        TickType_t now = xTaskGetTickCount();
        int peak = 0;
        int avg_abs = 0;
        analyze_pcm_level(data, len, &peak, &avg_abs);

        if (!s_continuous_chat || !mcp_client_is_connected()) {
            return;
        }
        if (s_audio_busy || mcp_client_is_assistant_busy()) {
            return;
        }
        if (now < s_cont_rearm_tick) {
            return;
        }
        if (!s_continuous_speaking) {
            int start_threshold = cont_vad_start_threshold();
            int stop_threshold = cont_vad_stop_threshold();
            bool avg_hit = avg_abs >= start_threshold;
            bool peak_hit = peak >= CONT_VAD_START_PEAK && avg_abs >= stop_threshold;
            bool voice_hit = avg_hit || peak_hit;
            if (voice_hit) {
                ++s_cont_start_hits;
            } else {
                cont_vad_update_noise_floor(avg_abs);
                if (s_cont_start_hits > 0) {
                    --s_cont_start_hits;
                }
            }
            cont_vad_log_sample(now, avg_abs, peak, 0);
            if (s_cont_start_hits < CONT_VAD_START_HITS) {
                return;
            }

            make_capture_session_id();
            s_capture_chunk_len = 0;
            esp_err_t err = mcp_client_audio_stream_begin(s_capture_session_id);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "continuous stream begin failed: %s", esp_err_to_name(err));
                s_capture_session_id[0] = '\0';
                s_cont_start_hits = 0;
                app_ui_set_mcp_status("MCP SEND FAIL");
                return;
            }
            s_continuous_speaking = true;
            s_cont_speech_start_tick = now;
            s_cont_last_voice_tick = now;
            s_cont_silence_hits = 0;
            app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
            app_ui_set_mic_state("MIC ON");
            ESP_LOGI(TAG,
                     "continuous utterance start avg=%d peak=%d noise=%d start=%d stop=%d",
                     avg_abs,
                     peak,
                     s_cont_noise_floor,
                     cont_vad_start_threshold(),
                     cont_vad_stop_threshold());
        }

        append_capture_audio(data, len);
        int stop_threshold = cont_vad_stop_threshold();
        bool voice_active = avg_abs >= stop_threshold;
        if (voice_active) {
            s_cont_silence_hits = 0;
            s_cont_last_voice_tick = now;
        } else {
            ++s_cont_silence_hits;
            if (s_cont_silence_hits < CONT_VAD_SILENCE_HITS) {
                s_cont_last_voice_tick = now;
            }
        }
        uint32_t speech_ms = (uint32_t)((now - s_cont_speech_start_tick) * portTICK_PERIOD_MS);
        uint32_t silence_ms = (uint32_t)((now - s_cont_last_voice_tick) * portTICK_PERIOD_MS);
        cont_vad_log_sample(now, avg_abs, peak, silence_ms);
        if ((speech_ms >= CONT_VAD_MIN_SPEECH_MS && silence_ms >= CONT_VAD_TAIL_MS) ||
            speech_ms >= CONT_VAD_MAX_SPEECH_MS) {
            finish_continuous_utterance(speech_ms >= CONT_VAD_MAX_SPEECH_MS ? "vad_max" : "vad_silence");
        }
        return;
    }

    append_capture_audio(data, len);
}

static void start_set_capture(void)
{
    if (s_audio_busy) {
        ESP_LOGW(TAG, "SET ignored: assistant audio busy");
        return;
    }
    if (s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        ESP_LOGW(TAG, "SET ignored: continuous chat is active");
        return;
    }
    if (!mcp_client_is_connected()) {
        ESP_LOGW(TAG, "SET ignored: MCP not connected");
        app_ui_set_mcp_status(mcp_client_get_status_text());
        app_ui_set_assistant_state(APP_UI_STATE_OFFLINE);
        return;
    }
    if (mic_diag_is_capturing()) {
        return;
    }

    make_capture_session_id();
    s_capture_chunk_len = 0;
    s_capture_mode = CAPTURE_MODE_PTT;

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
    if (s_capture_mode != CAPTURE_MODE_PTT || !mic_diag_is_capturing()) {
        return;
    }

    uint32_t duration_ms = mic_diag_capture_stop();
    flush_capture_chunk();
    mcp_client_audio_stream_end(s_capture_session_id, duration_ms, "set_release");
    s_capture_session_id[0] = '\0';
    s_capture_mode = CAPTURE_MODE_NONE;
    app_ui_set_assistant_state(APP_UI_STATE_UPLOADING);
    app_ui_set_mic_state("MIC READY");
}

static void start_continuous_chat(void)
{
    if (s_continuous_chat) {
        return;
    }
    if (!mcp_client_is_connected()) {
        app_ui_set_mcp_status(mcp_client_get_status_text());
        app_ui_set_assistant_state(APP_UI_STATE_OFFLINE);
        return;
    }
    if (mic_diag_is_capturing() && s_capture_mode == CAPTURE_MODE_PTT) {
        stop_set_capture();
    }
    s_continuous_chat = true;
    s_continuous_speaking = false;
    s_capture_mode = CAPTURE_MODE_CONTINUOUS;
    s_audio_busy = false;
    cont_vad_reset_runtime();
    cont_vad_reset_noise_floor();
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;

    esp_err_t err = mic_diag_capture_start(on_capture_audio, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "continuous mic start failed: %s", esp_err_to_name(err));
        s_capture_mode = CAPTURE_MODE_NONE;
        s_continuous_chat = false;
        app_ui_set_mic_state("MIC ERROR");
        app_ui_set_chat_continuous(false);
        return;
    }
    app_ui_set_chat_continuous(true);
    app_ui_set_assistant_state(APP_UI_STATE_IDLE);
    app_ui_set_mic_state("MIC ON");
    app_ui_set_voice_state("VOICE READY");
    send_runtime_config();
    ESP_LOGI(TAG, "continuous chat enabled");
}

static void stop_continuous_chat(void)
{
    if (!s_continuous_chat && s_capture_mode != CAPTURE_MODE_CONTINUOUS) {
        return;
    }
    if (s_continuous_speaking) {
        finish_continuous_utterance("manual_stop");
    }
    if (mic_diag_is_capturing() && s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        mic_diag_capture_stop();
    }
    s_continuous_chat = false;
    s_continuous_speaking = false;
    s_capture_mode = CAPTURE_MODE_NONE;
    s_audio_busy = false;
    cont_vad_reset_runtime();
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;
    app_ui_set_chat_continuous(false);
    app_ui_set_assistant_state(APP_UI_STATE_IDLE);
    app_ui_set_mic_state("MIC READY");
    send_runtime_config();
    ESP_LOGI(TAG, "continuous chat disabled");
}

static void toggle_continuous_chat(void)
{
    if (s_continuous_chat) {
        stop_continuous_chat();
    } else {
        start_continuous_chat();
    }
}

static void mark_assistant_audio_busy(bool busy)
{
    s_audio_busy = busy;
    if (!busy && s_continuous_chat) {
        s_cont_rearm_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CONT_REARM_DELAY_MS);
        cont_vad_reset_runtime();
    }
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
                mark_assistant_audio_busy(true);
                app_ui_set_assistant_state(APP_UI_STATE_PLAYING);
                audio_player_play_xiaole();
                app_ui_set_assistant_state(APP_UI_STATE_IDLE);
                mark_assistant_audio_busy(false);
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
                stop_continuous_chat();
                mcp_client_disconnect();
                app_ui_set_mcp_status(mcp_client_get_status_text());
                break;
            case VOICE_CMD_SET_PRESS:
                start_set_capture();
                break;
            case VOICE_CMD_SET_RELEASE:
                stop_set_capture();
                break;
            case VOICE_CMD_CHAT_TOGGLE:
                toggle_continuous_chat();
                break;
            case VOICE_CMD_WAKE_TOGGLE:
                s_wake_enabled = !s_wake_enabled;
                app_ui_set_wake_enabled(s_wake_enabled);
                app_ui_set_voice_state(s_wake_enabled ? "WAKE TODO" : "VOICE READY");
                send_runtime_config();
                ESP_LOGI(TAG, "wake word UI flag=%d; offline WakeNet not linked", s_wake_enabled);
                break;
            case VOICE_CMD_PERSONA_NEXT:
                s_persona_index = (s_persona_index + 1) % (sizeof(s_personas) / sizeof(s_personas[0]));
                app_ui_set_persona(s_personas[s_persona_index].label);
                send_runtime_config();
                ESP_LOGI(TAG, "persona=%s", s_personas[s_persona_index].id);
                break;
            case VOICE_CMD_VOICE_NEXT:
                s_voice_index = (s_voice_index + 1) % (sizeof(s_voice_profiles) / sizeof(s_voice_profiles[0]));
                app_ui_set_voice_profile(s_voice_profiles[s_voice_index].label);
                send_runtime_config();
                ESP_LOGI(TAG, "voice_profile=%s", s_voice_profiles[s_voice_index].id);
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
        } else if (strcmp(line, "CHAT") == 0) {
            send_cmd(VOICE_CMD_CHAT_TOGGLE, 0);
        } else if (strcmp(line, "WAKE") == 0) {
            send_cmd(VOICE_CMD_WAKE_TOGGLE, 0);
        } else if (strcmp(line, "PERSONA") == 0) {
            send_cmd(VOICE_CMD_PERSONA_NEXT, 0);
        } else if (strcmp(line, "VOICE") == 0) {
            send_cmd(VOICE_CMD_VOICE_NEXT, 0);
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
    app_ui_set_bluetooth_available(false);
    app_ui_set_chat_continuous(false);
    app_ui_set_wake_enabled(false);
    app_ui_set_persona(s_personas[s_persona_index].label);
    app_ui_set_voice_profile(s_voice_profiles[s_voice_index].label);

    if (mic_diag_init(on_mic_level) != ESP_OK) {
        ESP_LOGW(TAG, "mic diag init failed; MIC commands unavailable");
    }
    app_ui_set_mic_state("MIC READY");
    ESP_LOGI(TAG,
             "full duplex AFE/AEC experimental path=%d; current capture path is mono PCM",
             ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL);

    mcp_client_init();
    app_ui_set_mcp_status(mcp_client_get_status_text());
    send_runtime_config();

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
