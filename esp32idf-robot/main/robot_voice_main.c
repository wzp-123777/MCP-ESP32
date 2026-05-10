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
#include "afe_capture.h"
#include "afe_full_duplex_plan.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vad.h"
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
#define CONT_VAD_START_HITS 6
#define CONT_VAD_SILENCE_HITS 3
#define CONT_VAD_TAIL_MS 1400
#define CONT_VAD_MIN_SPEECH_MS 320
#define CONT_VAD_MAX_SPEECH_MS 12000
#define CONT_VAD_WATCHDOG_MS 300
#define CONT_VAD_STALE_AUDIO_MS 1600
#define CONT_VAD_STALE_SILENCE_MS 1400
#define CONT_REARM_DELAY_MS 3000
#define CONT_VAD_NOISE_FLOOR_INIT 140
#define CONT_VAD_NOISE_FLOOR_MIN 60
#define CONT_VAD_NOISE_FLOOR_MAX 460
#define CONT_VAD_START_MARGIN 380
#define CONT_VAD_STOP_MARGIN 170
#define CONT_VAD_LOG_INTERVAL_MS 1000
#define CONT_VAD_RELATIVE_RELEASE_DIV 3
#define CONT_SR_VAD_RATE 16000
#define CONT_SR_VAD_FRAME_MS 30
#define CONT_SR_VAD_FRAME_SAMPLES ((CONT_SR_VAD_RATE * CONT_SR_VAD_FRAME_MS) / 1000)
#define CONT_AEC_VAD_START_AVG 760
#define CONT_AEC_VAD_START_PEAK 4500
#define CONT_AEC_VAD_STOP_AVG 140
#define CONT_AEC_VAD_START_HITS 8
#define CONT_AEC_VAD_NOISE_FLOOR_INIT 120
#define CONT_AEC_VAD_NOISE_FLOOR_MIN 4
#define CONT_AEC_VAD_NOISE_FLOOR_MAX 300
#define CONT_AEC_VAD_START_MARGIN 420
#define CONT_AEC_VAD_STOP_MARGIN 90
#define CONT_AEC_VAD_RELATIVE_RELEASE_DIV 4
#define CONT_AFE_VAD_CONFIRM_MS 1200
#define CONT_AFE_VAD_CONFIRM_AVG 220
#define CONT_AFE_VAD_CONFIRM_PEAK 1000
#define CONT_AFE_VAD_CONFIRM_MARGIN 80
#define CONT_AFE_VAD_CONFIRM_STRONG_AVG 380
#define CONT_AFE_VAD_CONFIRM_STRONG_PEAK 2500
#define CONT_AFE_VAD_CONFIRM_STRONG_MARGIN 180
#define CONT_AFE_VAD_CONFIRM_HITS 3
#define CONT_AFE_LOCAL_START_AVG 280
#define CONT_AFE_LOCAL_START_PEAK 1400
#define CONT_AFE_LOCAL_START_MARGIN 120
#define CONT_AFE_LOCAL_START_HITS 5
#define CONT_AFE_REJECT_REARM_MS 1200
#define CONT_PREROLL_CHUNKS 16
#define RAW_TDM_DIAG_CHANNELS 4
#define RAW_TDM_DIAG_RATE 16000
#define RAW_TDM_DIAG_DEFAULT_MS 3000
#define RAW_TDM_DIAG_MIN_MS 500
#define RAW_TDM_DIAG_MAX_MS 5000

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
    VOICE_CMD_RAW_TDM_DIAG,
    VOICE_CMD_RAW_TDM_DONE,
} voice_cmd_type_t;

typedef struct {
    voice_cmd_type_t type;
    int value;
} voice_cmd_t;

typedef struct {
    int peak;
    int64_t sum_abs;
    uint64_t sum_sq;
    uint32_t samples;
    uint32_t zero_crossings;
    int16_t prev;
    bool prev_valid;
} raw_tdm_channel_stats_t;

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
static TickType_t s_cont_last_audio_tick;
static TickType_t s_cont_rearm_tick;
static TickType_t s_cont_vad_log_tick;
static TickType_t s_cont_busy_log_tick;
static int s_cont_start_hits;
static int s_cont_silence_hits;
static int s_cont_noise_floor = CONT_VAD_NOISE_FLOOR_INIT;
static int s_cont_utterance_peak_avg;
static uint8_t *s_cont_preroll_buf;
static size_t s_cont_preroll_lens[CONT_PREROLL_CHUNKS];
static size_t s_cont_preroll_head;
static size_t s_cont_preroll_count;
static vad_handle_t s_cont_sr_vad;
static int16_t s_cont_sr_vad_frame[CONT_SR_VAD_FRAME_SAMPLES];
static int s_cont_sr_vad_frame_used;
static bool s_cont_sr_vad_available;
static bool s_afe_ready;
static bool s_afe_init_attempted;
static bool s_afe_vad_pending;
static TickType_t s_afe_vad_pending_tick;
static TickType_t s_afe_vad_reject_until_tick;
static int s_afe_vad_confirm_hits;
static bool s_mic_diag_ready;
static bool s_mic_diag_init_attempted;
static volatile bool s_raw_tdm_active;
static volatile bool s_raw_tdm_finish_queued;
static TickType_t s_raw_tdm_start_tick;
static TickType_t s_raw_tdm_end_tick;
static uint32_t s_raw_tdm_target_ms;
static int s_raw_tdm_channels_seen;
static uint8_t *s_raw_tdm_channel_bufs[RAW_TDM_DIAG_CHANNELS];
static size_t s_raw_tdm_channel_lens[RAW_TDM_DIAG_CHANNELS];
static size_t s_raw_tdm_channel_cap;
static raw_tdm_channel_stats_t s_raw_tdm_stats[RAW_TDM_DIAG_CHANNELS];
static uint8_t *s_raw_tdm_afe_buf;
static size_t s_raw_tdm_afe_len;
static raw_tdm_channel_stats_t s_raw_tdm_afe_stats;

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

static void make_capture_session_id(void);
static void finish_continuous_utterance(const char *reason);
static void clear_afe_vad_pending(void);
static void on_mcp_device_command(const char *command, void *ctx);
static void start_raw_tdm_diag(int duration_ms);
static void finish_raw_tdm_diag(void);
static void raw_tdm_diag_housekeeping(void);

static void print_help(void)
{
    ESP_LOGI(TAG, "commands: ASK <text>, REC <ms>, CHAT, WAKE, RAW TDM [ms], PERSONA, VOICE, PLAY/XIAOLE, LOOP, STOP, MIC ON, MIC OFF, VOL 0-100, VOL+, VOL-, MCP URL <url>, MCP CONNECT, HELP");
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

static void on_afe_event(afe_capture_event_t event, void *ctx)
{
    (void)ctx;
    switch (event) {
        case AFE_CAPTURE_EVENT_VAD_START:
            ESP_LOGI(TAG, "afe vad start");
            if (s_capture_mode == CAPTURE_MODE_CONTINUOUS &&
                s_continuous_chat &&
                !s_continuous_speaking &&
                mcp_client_is_connected() &&
                !s_audio_busy &&
                !mcp_client_is_assistant_busy()) {
                TickType_t now = xTaskGetTickCount();
                if (now >= s_cont_rearm_tick) {
                    s_afe_vad_pending = true;
                    s_afe_vad_pending_tick = now;
                    s_afe_vad_confirm_hits = 0;
                    ESP_LOGI(TAG, "afe vad pending energy confirm");
                }
            }
            break;
        case AFE_CAPTURE_EVENT_VAD_END:
            if (s_capture_mode == CAPTURE_MODE_CONTINUOUS && s_continuous_speaking) {
                s_cont_last_voice_tick = xTaskGetTickCount();
                ESP_LOGI(TAG, "afe vad end; wait for local tail window");
            } else if (s_afe_vad_pending) {
                ESP_LOGI(TAG, "afe vad dropped before energy confirm");
                s_afe_vad_reject_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS);
                clear_afe_vad_pending();
            }
            break;
        case AFE_CAPTURE_EVENT_WAKE:
            ESP_LOGI(TAG, "afe wake");
            if (s_wake_enabled && !s_continuous_chat) {
                send_cmd_nonblocking(VOICE_CMD_CHAT_TOGGLE, 0);
            }
            break;
        case AFE_CAPTURE_EVENT_ERROR:
            ESP_LOGW(TAG, "afe capture error");
            app_ui_set_assistant_state(APP_UI_STATE_ERROR);
            break;
    }
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

static bool ensure_afe_ready(void)
{
    if (s_afe_ready) {
        return true;
    }
    if (!ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL || s_afe_init_attempted) {
        return false;
    }
    s_afe_init_attempted = true;
    esp_err_t afe_err = afe_capture_init(on_afe_event, NULL);
    s_afe_ready = afe_err == ESP_OK;
    mcp_client_set_sr_enabled(s_afe_ready);
    if (!s_afe_ready) {
        ESP_LOGW(TAG, "AFE/AEC init failed: %s; capture falls back to mono PCM",
                 esp_err_to_name(afe_err));
    } else {
        ESP_LOGI(TAG, "AFE/AEC ready; capture path=AFE/AEC");
    }
    return s_afe_ready;
}

static bool ensure_mic_diag_ready(void)
{
    if (s_mic_diag_ready) {
        return true;
    }
    if (s_mic_diag_init_attempted) {
        return false;
    }
    s_mic_diag_init_attempted = true;
    s_mic_diag_ready = mic_diag_init(on_mic_level) == ESP_OK;
    if (!s_mic_diag_ready) {
        ESP_LOGW(TAG, "mic diag init failed; MIC commands unavailable");
    }
    return s_mic_diag_ready;
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

static bool cont_using_aec_path(void)
{
    return s_afe_ready && s_capture_mode == CAPTURE_MODE_CONTINUOUS;
}

static bool cont_preroll_ensure(void)
{
    if (s_cont_preroll_buf) {
        return true;
    }
    s_cont_preroll_buf = heap_caps_malloc(CONT_PREROLL_CHUNKS * CAPTURE_CHUNK_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cont_preroll_buf) {
        ESP_LOGW(TAG, "continuous preroll buffer alloc failed");
        return false;
    }
    s_cont_preroll_head = 0;
    s_cont_preroll_count = 0;
    memset(s_cont_preroll_lens, 0, sizeof(s_cont_preroll_lens));
    return true;
}

static void cont_preroll_reset(void)
{
    s_cont_preroll_head = 0;
    s_cont_preroll_count = 0;
    memset(s_cont_preroll_lens, 0, sizeof(s_cont_preroll_lens));
}

static void cont_preroll_store(const uint8_t *data, int len)
{
    if (!data || len <= 0 || !cont_preroll_ensure()) {
        return;
    }

    int offset = 0;
    while (offset < len) {
        size_t copy_len = (size_t)(len - offset);
        if (copy_len > CAPTURE_CHUNK_BYTES) {
            copy_len = CAPTURE_CHUNK_BYTES;
        }
        size_t slot = s_cont_preroll_head;
        memcpy(s_cont_preroll_buf + slot * CAPTURE_CHUNK_BYTES, data + offset, copy_len);
        s_cont_preroll_lens[slot] = copy_len;
        s_cont_preroll_head = (s_cont_preroll_head + 1) % CONT_PREROLL_CHUNKS;
        if (s_cont_preroll_count < CONT_PREROLL_CHUNKS) {
            ++s_cont_preroll_count;
        }
        offset += (int)copy_len;
    }
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

static void cont_sr_vad_reset(void)
{
    s_cont_sr_vad_frame_used = 0;
    if (s_cont_sr_vad) {
        vad_reset_trigger(s_cont_sr_vad);
    }
}

static bool cont_sr_vad_ensure(void)
{
    if (s_cont_sr_vad_available) {
        return s_cont_sr_vad != NULL;
    }
    s_cont_sr_vad_available = true;
    s_cont_sr_vad = vad_create_with_param(VAD_MODE_2,
                                          CONT_SR_VAD_RATE,
                                          CONT_SR_VAD_FRAME_MS,
                                          120,
                                          650);
    if (!s_cont_sr_vad) {
        ESP_LOGW(TAG, "ESP-SR VAD unavailable; use energy VAD only");
        return false;
    }
    ESP_LOGI(TAG, "ESP-SR VAD ready mode=2 frame=%dms", CONT_SR_VAD_FRAME_MS);
    return true;
}

static vad_state_t cont_sr_vad_process(const uint8_t *data, int len)
{
    if (!cont_sr_vad_ensure() || !data || len <= 0) {
        return VAD_SILENCE;
    }

    bool speech_seen = false;
    const int16_t *samples = (const int16_t *)data;
    int sample_count = len / (int)sizeof(int16_t);
    for (int i = 0; i < sample_count; ++i) {
        s_cont_sr_vad_frame[s_cont_sr_vad_frame_used++] = samples[i];
        if (s_cont_sr_vad_frame_used >= CONT_SR_VAD_FRAME_SAMPLES) {
            vad_state_t state = vad_process_with_trigger(s_cont_sr_vad, s_cont_sr_vad_frame);
            if (state == VAD_SPEECH) {
                speech_seen = true;
            }
            s_cont_sr_vad_frame_used = 0;
        }
    }
    return speech_seen ? VAD_SPEECH : VAD_SILENCE;
}

static int cont_vad_start_threshold(void)
{
    if (cont_using_aec_path()) {
        return max_int(CONT_AEC_VAD_START_AVG, s_cont_noise_floor + CONT_AEC_VAD_START_MARGIN);
    }
    return max_int(CONT_VAD_START_AVG, s_cont_noise_floor + CONT_VAD_START_MARGIN);
}

static int cont_vad_stop_threshold(void)
{
    if (cont_using_aec_path()) {
        return max_int(CONT_AEC_VAD_STOP_AVG, s_cont_noise_floor + CONT_AEC_VAD_STOP_MARGIN);
    }
    return max_int(CONT_VAD_STOP_AVG, s_cont_noise_floor + CONT_VAD_STOP_MARGIN);
}

static void cont_vad_reset_runtime(void)
{
    s_cont_start_hits = 0;
    s_cont_silence_hits = 0;
    s_cont_vad_log_tick = 0;
    s_cont_busy_log_tick = 0;
    s_cont_utterance_peak_avg = 0;
    s_cont_last_audio_tick = 0;
    clear_afe_vad_pending();
    s_afe_vad_reject_until_tick = 0;
    cont_sr_vad_reset();
}

static void clear_afe_vad_pending(void)
{
    s_afe_vad_pending = false;
    s_afe_vad_pending_tick = 0;
    s_afe_vad_confirm_hits = 0;
}

static void cont_vad_reset_noise_floor(void)
{
    s_cont_noise_floor = cont_using_aec_path() ? CONT_AEC_VAD_NOISE_FLOOR_INIT : CONT_VAD_NOISE_FLOOR_INIT;
}

static void cont_vad_update_noise_floor(int avg_abs)
{
    int start_threshold = cont_vad_start_threshold();
    if (avg_abs >= start_threshold) {
        return;
    }

    int floor_min = cont_using_aec_path() ? CONT_AEC_VAD_NOISE_FLOOR_MIN : CONT_VAD_NOISE_FLOOR_MIN;
    int floor_max = cont_using_aec_path() ? CONT_AEC_VAD_NOISE_FLOOR_MAX : CONT_VAD_NOISE_FLOOR_MAX;
    int sample = clamp_int(avg_abs, floor_min, floor_max);
    s_cont_noise_floor = ((s_cont_noise_floor * 15) + sample) / 16;
    s_cont_noise_floor = clamp_int(s_cont_noise_floor, floor_min, floor_max);
}

static void cont_vad_log_sample_ex(TickType_t now,
                                   int avg_abs,
                                   int peak,
                                   uint32_t silence_ms,
                                   vad_state_t sr_vad_state)
{
    if ((uint32_t)((now - s_cont_vad_log_tick) * portTICK_PERIOD_MS) < CONT_VAD_LOG_INTERVAL_MS) {
        return;
    }
    s_cont_vad_log_tick = now;
    ESP_LOGI(TAG,
             "cont vad avg=%d peak=%d noise=%d start=%d stop=%d speaking=%d silent_hits=%d silence=%ums sr=%d",
             avg_abs,
             peak,
             s_cont_noise_floor,
             cont_vad_start_threshold(),
             cont_vad_stop_threshold(),
             s_continuous_speaking,
             s_cont_silence_hits,
             (unsigned)silence_ms,
             sr_vad_state == VAD_SPEECH ? 1 : 0);
}

static void cont_vad_log_busy_gate(TickType_t now)
{
    if ((uint32_t)((now - s_cont_busy_log_tick) * portTICK_PERIOD_MS) < CONT_VAD_LOG_INTERVAL_MS) {
        return;
    }
    s_cont_busy_log_tick = now;
    ESP_LOGI(TAG,
             "cont vad gated busy local=%d mcp=%d rearm_left=%dms",
             s_audio_busy,
             mcp_client_is_assistant_busy(),
             now < s_cont_rearm_tick ? (int)((s_cont_rearm_tick - now) * portTICK_PERIOD_MS) : 0);
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

static void cont_preroll_flush_to_capture(void)
{
    if (!s_cont_preroll_buf || s_cont_preroll_count == 0 || s_capture_session_id[0] == '\0') {
        cont_preroll_reset();
        return;
    }
    size_t start = (s_cont_preroll_head + CONT_PREROLL_CHUNKS - s_cont_preroll_count) % CONT_PREROLL_CHUNKS;
    for (size_t i = 0; i < s_cont_preroll_count; ++i) {
        size_t slot = (start + i) % CONT_PREROLL_CHUNKS;
        size_t len = s_cont_preroll_lens[slot];
        if (len > 0) {
            append_capture_audio(s_cont_preroll_buf + slot * CAPTURE_CHUNK_BYTES, (int)len);
        }
    }
    cont_preroll_reset();
}

static void finish_continuous_utterance(const char *reason)
{
    if (!s_continuous_speaking) {
        return;
    }
    bool abort_upload = reason && (strcmp(reason, "manual_stop") == 0 ||
                                   strcmp(reason, "mcp_disconnect") == 0);
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
    cont_preroll_reset();
    s_audio_busy = !abort_upload;
    s_cont_rearm_tick = now + pdMS_TO_TICKS(CONT_REARM_DELAY_MS);
    app_ui_set_assistant_state(strcmp(reason ? reason : "", "mcp_disconnect") == 0
                                   ? APP_UI_STATE_OFFLINE
                                   : (abort_upload ? APP_UI_STATE_IDLE : APP_UI_STATE_UPLOADING));
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
        s_cont_last_audio_tick = now;
        int peak = 0;
        int avg_abs = 0;
        analyze_pcm_level(data, len, &peak, &avg_abs);
        bool afe_vad_path = cont_using_aec_path();
        vad_state_t sr_vad_state = cont_sr_vad_process(data, len);

        if (!s_continuous_chat) {
            return;
        }
        if (!mcp_client_is_connected()) {
            if (s_continuous_speaking) {
                finish_continuous_utterance("mcp_disconnect");
            }
            return;
        }
        bool assistant_busy = s_audio_busy || mcp_client_is_assistant_busy();
        if (!s_continuous_speaking && assistant_busy) {
            cont_vad_log_busy_gate(now);
            return;
        }
        if (!s_continuous_speaking && now < s_cont_rearm_tick) {
            cont_vad_log_busy_gate(now);
            return;
        }
        bool started_now = false;
        if (afe_vad_path && !s_continuous_speaking) {
            cont_preroll_store(data, len);
            cont_vad_update_noise_floor(avg_abs);
            cont_vad_log_sample_ex(now, avg_abs, peak, 0, sr_vad_state);
            if (s_afe_vad_pending) {
                uint32_t pending_ms = (uint32_t)((now - s_afe_vad_pending_tick) * portTICK_PERIOD_MS);
                if (pending_ms > CONT_AFE_VAD_CONFIRM_MS) {
                    ESP_LOGI(TAG, "afe vad confirm timeout avg=%d peak=%d", avg_abs, peak);
                    s_afe_vad_reject_until_tick = now + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS);
                    clear_afe_vad_pending();
                    return;
                }
                int confirm_avg = max_int(CONT_AFE_VAD_CONFIRM_AVG,
                                          s_cont_noise_floor + CONT_AFE_VAD_CONFIRM_MARGIN);
                int strong_avg = max_int(CONT_AFE_VAD_CONFIRM_STRONG_AVG,
                                         s_cont_noise_floor + CONT_AFE_VAD_CONFIRM_STRONG_MARGIN);
                bool sr_energy_hit = (sr_vad_state == VAD_SPEECH) &&
                                     avg_abs >= confirm_avg &&
                                     peak >= CONT_AFE_VAD_CONFIRM_PEAK;
                bool strong_energy_hit = avg_abs >= strong_avg &&
                                         peak >= CONT_AFE_VAD_CONFIRM_STRONG_PEAK;
                if (sr_energy_hit || strong_energy_hit) {
                    ++s_afe_vad_confirm_hits;
                } else if (s_afe_vad_confirm_hits > 0) {
                    --s_afe_vad_confirm_hits;
                }
                if (s_afe_vad_confirm_hits < CONT_AFE_VAD_CONFIRM_HITS) {
                    return;
                }

                make_capture_session_id();
                s_capture_chunk_len = 0;
                esp_err_t err = mcp_client_audio_stream_begin(s_capture_session_id);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "afe stream begin failed: %s", esp_err_to_name(err));
                    s_capture_session_id[0] = '\0';
                    clear_afe_vad_pending();
                    app_ui_set_mcp_status("MCP SEND FAIL");
                    return;
                }
                s_continuous_speaking = true;
                s_cont_speech_start_tick = now;
                s_cont_last_voice_tick = now;
                s_cont_last_audio_tick = now;
                s_cont_silence_hits = 0;
                s_cont_utterance_peak_avg = avg_abs;
                cont_preroll_flush_to_capture();
                started_now = true;
                clear_afe_vad_pending();
                app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
                app_ui_set_mic_state("AFE REC");
                ESP_LOGI(TAG,
                         "afe utterance start session=%s avg=%d peak=%d confirm_avg=%d strong_avg=%d hits=%d sr=%d",
                         s_capture_session_id,
                         avg_abs,
                         peak,
                         confirm_avg,
                         strong_avg,
                         CONT_AFE_VAD_CONFIRM_HITS,
                         sr_vad_state == VAD_SPEECH ? 1 : 0);
            }
        }
        if (afe_vad_path && !s_continuous_speaking && now < s_afe_vad_reject_until_tick) {
            return;
        }
        if (!s_continuous_speaking) {
            if (!afe_vad_path) {
                cont_preroll_store(data, len);
            }
            int start_threshold = cont_vad_start_threshold();
            int start_peak = cont_using_aec_path() ? CONT_AEC_VAD_START_PEAK : CONT_VAD_START_PEAK;
            int start_hits = cont_using_aec_path() ? CONT_AEC_VAD_START_HITS : CONT_VAD_START_HITS;
            bool sr_hit = false;
            bool voice_hit = false;
            if (cont_using_aec_path()) {
                start_threshold = max_int(CONT_AFE_LOCAL_START_AVG,
                                          s_cont_noise_floor + CONT_AFE_LOCAL_START_MARGIN);
                start_peak = CONT_AFE_LOCAL_START_PEAK;
                start_hits = CONT_AFE_LOCAL_START_HITS;
                int strong_avg = max_int(CONT_AFE_VAD_CONFIRM_STRONG_AVG,
                                         s_cont_noise_floor + CONT_AFE_VAD_CONFIRM_STRONG_MARGIN);
                sr_hit = (sr_vad_state == VAD_SPEECH) &&
                         avg_abs >= start_threshold &&
                         peak >= start_peak;
                bool strong_energy_hit = avg_abs >= strong_avg &&
                                         peak >= CONT_AFE_VAD_CONFIRM_STRONG_PEAK;
                voice_hit = strong_energy_hit || sr_hit;
            } else {
                bool avg_hit = avg_abs >= start_threshold;
                bool peak_hit = peak >= start_peak && avg_abs >= start_threshold;
                sr_hit = sr_vad_state == VAD_SPEECH;
                voice_hit = sr_hit || avg_hit || peak_hit;
            }
            if (voice_hit) {
                ++s_cont_start_hits;
            } else {
                cont_vad_update_noise_floor(avg_abs);
                if (cont_using_aec_path()) {
                    s_cont_start_hits = 0;
                } else if (s_cont_start_hits > 0) {
                    --s_cont_start_hits;
                }
            }
            cont_vad_log_sample_ex(now, avg_abs, peak, 0, sr_vad_state);
            if (s_cont_start_hits < start_hits) {
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
            s_cont_last_audio_tick = now;
            s_cont_silence_hits = 0;
            s_cont_utterance_peak_avg = avg_abs;
            cont_preroll_flush_to_capture();
            started_now = true;
            app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
            app_ui_set_mic_state("MIC ON");
            ESP_LOGI(TAG,
                     "continuous utterance start avg=%d peak=%d noise=%d start=%d stop=%d hits=%d sr=%d",
                     avg_abs,
                     peak,
                     s_cont_noise_floor,
                     cont_vad_start_threshold(),
                     cont_vad_stop_threshold(),
                     s_cont_start_hits,
                     sr_vad_state == VAD_SPEECH ? 1 : 0);
        }

        if (!started_now) {
            append_capture_audio(data, len);
        }
        int stop_threshold = cont_vad_stop_threshold();
        if (avg_abs > s_cont_utterance_peak_avg) {
            s_cont_utterance_peak_avg = avg_abs;
        }
        int release_floor = cont_using_aec_path() ? CONT_AEC_VAD_STOP_AVG : CONT_VAD_STOP_AVG;
        int release_div = cont_using_aec_path() ? CONT_AEC_VAD_RELATIVE_RELEASE_DIV : CONT_VAD_RELATIVE_RELEASE_DIV;
        int relative_release = max_int(release_floor, s_cont_utterance_peak_avg / release_div);
        bool energy_active = avg_abs >= stop_threshold && avg_abs >= relative_release;
        bool strong_energy_active = avg_abs >= max_int(stop_threshold + 80, relative_release + 80);
        bool voice_active = energy_active;
        if (cont_using_aec_path() && s_cont_sr_vad) {
            bool sr_energy_active = (sr_vad_state == VAD_SPEECH) &&
                                    avg_abs >= stop_threshold &&
                                    peak >= CONT_AFE_VAD_CONFIRM_PEAK;
            voice_active = sr_energy_active || strong_energy_active;
        }
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
        cont_vad_log_sample_ex(now, avg_abs, peak, silence_ms, sr_vad_state);
        if ((speech_ms >= CONT_VAD_MIN_SPEECH_MS && silence_ms >= CONT_VAD_TAIL_MS) ||
            speech_ms >= CONT_VAD_MAX_SPEECH_MS) {
            finish_continuous_utterance(speech_ms >= CONT_VAD_MAX_SPEECH_MS ? "vad_max" : "vad_silence");
        }
        return;
    }

    append_capture_audio(data, len);
}

static void continuous_chat_housekeeping(void)
{
    if (!s_continuous_chat || !s_continuous_speaking) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (!mcp_client_is_connected()) {
        finish_continuous_utterance("mcp_disconnect");
        return;
    }

    uint32_t speech_ms = (uint32_t)((now - s_cont_speech_start_tick) * portTICK_PERIOD_MS);
    uint32_t silence_ms = (uint32_t)((now - s_cont_last_voice_tick) * portTICK_PERIOD_MS);
    uint32_t stale_audio_ms = s_cont_last_audio_tick
                                  ? (uint32_t)((now - s_cont_last_audio_tick) * portTICK_PERIOD_MS)
                                  : speech_ms;
    if (speech_ms >= CONT_VAD_MIN_SPEECH_MS &&
        stale_audio_ms >= CONT_VAD_STALE_AUDIO_MS &&
        (silence_ms >= CONT_VAD_STALE_SILENCE_MS || speech_ms >= CONT_VAD_MAX_SPEECH_MS)) {
        ESP_LOGW(TAG,
                 "continuous watchdog end reason=vad_stale duration=%ums silence=%ums stale_audio=%ums",
                 (unsigned)speech_ms,
                 (unsigned)silence_ms,
                 (unsigned)stale_audio_ms);
        finish_continuous_utterance("vad_stale");
    }
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
    ensure_afe_ready();
    if ((s_afe_ready && afe_capture_is_running()) ||
        (!s_afe_ready && s_mic_diag_ready && mic_diag_is_capturing())) {
        return;
    }
    if (!s_afe_ready && !ensure_mic_diag_ready()) {
        app_ui_set_mic_state("MIC ERROR");
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

    err = s_afe_ready
              ? afe_capture_start_forced(on_capture_audio, NULL)
              : mic_diag_capture_start(on_capture_audio, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mic capture start failed: %s", esp_err_to_name(err));
        mcp_client_audio_stream_end(s_capture_session_id, 0, "mic_start_failed");
        app_ui_set_mic_state("MIC ERROR");
        return;
    }

    app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
    app_ui_set_mic_state(s_afe_ready ? "AFE SET" : "REC SET");
}

static void stop_set_capture(void)
{
    if (s_capture_mode != CAPTURE_MODE_PTT ||
        (s_afe_ready ? !afe_capture_is_running() : (!s_mic_diag_ready || !mic_diag_is_capturing()))) {
        return;
    }

    uint32_t duration_ms = s_afe_ready ? afe_capture_stop() : mic_diag_capture_stop();
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
    ensure_afe_ready();
    if ((s_afe_ready && afe_capture_is_running() && s_capture_mode == CAPTURE_MODE_PTT) ||
        (!s_afe_ready && s_mic_diag_ready && mic_diag_is_capturing() && s_capture_mode == CAPTURE_MODE_PTT)) {
        stop_set_capture();
    }
    s_continuous_chat = true;
    s_continuous_speaking = false;
    s_capture_mode = CAPTURE_MODE_CONTINUOUS;
    s_audio_busy = false;
    cont_vad_reset_runtime();
    cont_vad_reset_noise_floor();
    cont_preroll_reset();
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;

    esp_err_t err = s_afe_ready
                        ? afe_capture_start(on_capture_audio, NULL)
                        : mic_diag_capture_start(on_capture_audio, NULL);
    if (err != ESP_OK) {
        if (!s_afe_ready && !s_mic_diag_ready && ensure_mic_diag_ready()) {
            err = mic_diag_capture_start(on_capture_audio, NULL);
        }
    }
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
    app_ui_set_mic_state(s_afe_ready ? "AFE ON" : "MIC ON");
    app_ui_set_voice_state("VOICE READY");
    send_runtime_config();
    ESP_LOGI(TAG, "continuous chat enabled path=%s", s_afe_ready ? "afe_aec" : "mono_pcm");
}

static void stop_continuous_chat(void)
{
    if (!s_continuous_chat && s_capture_mode != CAPTURE_MODE_CONTINUOUS) {
        return;
    }
    if (s_continuous_speaking) {
        finish_continuous_utterance("manual_stop");
    }
    if (s_afe_ready && afe_capture_is_running() && s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        afe_capture_stop();
    } else if (s_mic_diag_ready && mic_diag_is_capturing() && s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        mic_diag_capture_stop();
    }
    s_continuous_chat = false;
    s_continuous_speaking = false;
    s_capture_mode = CAPTURE_MODE_NONE;
    s_audio_busy = false;
    cont_vad_reset_runtime();
    cont_preroll_reset();
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;
    app_ui_set_chat_continuous(false);
    app_ui_set_assistant_state(APP_UI_STATE_IDLE);
    app_ui_set_mic_state("MIC READY");
    send_runtime_config();
    ESP_LOGI(TAG, "continuous chat disabled");
}

static uint32_t isqrt_u64_local(uint64_t value)
{
    uint64_t bit = (uint64_t)1 << 62;
    uint64_t result = 0;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

static void raw_tdm_free_buffers(void)
{
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        if (s_raw_tdm_channel_bufs[ch]) {
            heap_caps_free(s_raw_tdm_channel_bufs[ch]);
            s_raw_tdm_channel_bufs[ch] = NULL;
        }
        s_raw_tdm_channel_lens[ch] = 0;
    }
    if (s_raw_tdm_afe_buf) {
        heap_caps_free(s_raw_tdm_afe_buf);
        s_raw_tdm_afe_buf = NULL;
    }
    s_raw_tdm_afe_len = 0;
    s_raw_tdm_channel_cap = 0;
}

static void raw_tdm_reset_state(void)
{
    s_raw_tdm_active = false;
    s_raw_tdm_finish_queued = false;
    s_raw_tdm_start_tick = 0;
    s_raw_tdm_end_tick = 0;
    s_raw_tdm_target_ms = 0;
    s_raw_tdm_channels_seen = 0;
    memset(s_raw_tdm_stats, 0, sizeof(s_raw_tdm_stats));
    memset(s_raw_tdm_channel_lens, 0, sizeof(s_raw_tdm_channel_lens));
    memset(&s_raw_tdm_afe_stats, 0, sizeof(s_raw_tdm_afe_stats));
    s_raw_tdm_afe_len = 0;
}

static void queue_raw_tdm_finish(void)
{
    if (!s_raw_tdm_active || s_raw_tdm_finish_queued) {
        return;
    }
    s_raw_tdm_finish_queued = true;
    send_cmd_nonblocking(VOICE_CMD_RAW_TDM_DONE, 0);
}

static void on_raw_tdm_audio(const int16_t *interleaved, int frames, int channels, void *ctx)
{
    (void)ctx;
    if (!s_raw_tdm_active || !interleaved || frames <= 0 || channels <= 0) {
        return;
    }

    int channel_count = channels < RAW_TDM_DIAG_CHANNELS ? channels : RAW_TDM_DIAG_CHANNELS;
    s_raw_tdm_channels_seen = channels;

    for (int frame = 0; frame < frames; ++frame) {
        const int16_t *row = interleaved + frame * channels;
        for (int ch = 0; ch < channel_count; ++ch) {
            int16_t sample = row[ch];
            int level = abs16_sample(sample);
            raw_tdm_channel_stats_t *stats = &s_raw_tdm_stats[ch];
            if (level > stats->peak) {
                stats->peak = level;
            }
            stats->sum_abs += level;
            stats->sum_sq += (uint64_t)((int32_t)sample * (int32_t)sample);
            if (stats->prev_valid && ((sample < 0) != (stats->prev < 0))) {
                ++stats->zero_crossings;
            }
            stats->prev = sample;
            stats->prev_valid = true;
            ++stats->samples;

            size_t offset = s_raw_tdm_channel_lens[ch];
            if (s_raw_tdm_channel_bufs[ch] && offset + sizeof(sample) <= s_raw_tdm_channel_cap) {
                memcpy(s_raw_tdm_channel_bufs[ch] + offset, &sample, sizeof(sample));
                s_raw_tdm_channel_lens[ch] = offset + sizeof(sample);
            }
        }
    }

    TickType_t now = xTaskGetTickCount();
    if (now >= s_raw_tdm_end_tick) {
        queue_raw_tdm_finish();
        return;
    }
    for (int ch = 0; ch < channel_count; ++ch) {
        if (s_raw_tdm_channel_lens[ch] >= s_raw_tdm_channel_cap) {
            queue_raw_tdm_finish();
            return;
        }
    }
}

static void on_raw_tdm_afe_audio(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    if (!s_raw_tdm_active || !data || len <= 0 || !s_raw_tdm_afe_buf || s_raw_tdm_channel_cap == 0) {
        return;
    }

    int sample_count = (len & ~(int)1) / (int)sizeof(int16_t);
    const int16_t *samples = (const int16_t *)data;
    for (int i = 0; i < sample_count; ++i) {
        int16_t sample = samples[i];
        int level = abs16_sample(sample);
        if (level > s_raw_tdm_afe_stats.peak) {
            s_raw_tdm_afe_stats.peak = level;
        }
        s_raw_tdm_afe_stats.sum_abs += level;
        s_raw_tdm_afe_stats.sum_sq += (uint64_t)((int32_t)sample * (int32_t)sample);
        if (s_raw_tdm_afe_stats.prev_valid &&
            ((sample < 0) != (s_raw_tdm_afe_stats.prev < 0))) {
            ++s_raw_tdm_afe_stats.zero_crossings;
        }
        s_raw_tdm_afe_stats.prev = sample;
        s_raw_tdm_afe_stats.prev_valid = true;
        ++s_raw_tdm_afe_stats.samples;

        size_t offset = s_raw_tdm_afe_len;
        if (offset + sizeof(sample) <= s_raw_tdm_channel_cap) {
            memcpy(s_raw_tdm_afe_buf + offset, &sample, sizeof(sample));
            s_raw_tdm_afe_len = offset + sizeof(sample);
        }
    }
}

static void start_raw_tdm_diag(int duration_ms)
{
    if (s_raw_tdm_active) {
        ESP_LOGW(TAG, "raw TDM diag already active");
        return;
    }

    duration_ms = clamp_int(duration_ms, RAW_TDM_DIAG_MIN_MS, RAW_TDM_DIAG_MAX_MS);
    if (s_continuous_chat || s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        stop_continuous_chat();
    }
    if (s_capture_mode == CAPTURE_MODE_PTT) {
        stop_set_capture();
    }
    if (!ensure_afe_ready()) {
        ESP_LOGW(TAG, "raw TDM diag requires AFE raw feed");
        app_ui_set_mic_state("RAW DIAG ERR");
        return;
    }

    raw_tdm_free_buffers();
    raw_tdm_reset_state();
    s_raw_tdm_channel_cap = (size_t)RAW_TDM_DIAG_RATE * (size_t)duration_ms * sizeof(int16_t) / 1000;
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        s_raw_tdm_channel_bufs[ch] = heap_caps_malloc(s_raw_tdm_channel_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_raw_tdm_channel_bufs[ch]) {
            s_raw_tdm_channel_bufs[ch] = heap_caps_malloc(s_raw_tdm_channel_cap, MALLOC_CAP_8BIT);
        }
        if (!s_raw_tdm_channel_bufs[ch]) {
            ESP_LOGE(TAG, "raw TDM diag buffer alloc failed ch=%d bytes=%u", ch, (unsigned)s_raw_tdm_channel_cap);
            raw_tdm_free_buffers();
            app_ui_set_mic_state("RAW DIAG OOM");
            return;
        }
    }
    s_raw_tdm_afe_buf = heap_caps_malloc(s_raw_tdm_channel_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_raw_tdm_afe_buf) {
        s_raw_tdm_afe_buf = heap_caps_malloc(s_raw_tdm_channel_cap, MALLOC_CAP_8BIT);
    }
    if (!s_raw_tdm_afe_buf) {
        ESP_LOGE(TAG, "raw TDM diag AFE buffer alloc failed bytes=%u", (unsigned)s_raw_tdm_channel_cap);
        raw_tdm_free_buffers();
        app_ui_set_mic_state("RAW DIAG OOM");
        return;
    }

    s_raw_tdm_target_ms = (uint32_t)duration_ms;
    s_raw_tdm_start_tick = xTaskGetTickCount();
    s_raw_tdm_end_tick = s_raw_tdm_start_tick + pdMS_TO_TICKS(duration_ms);
    s_raw_tdm_active = true;
    s_raw_tdm_finish_queued = false;
    afe_capture_set_raw_audio_callback(on_raw_tdm_audio, NULL);
    afe_capture_set_processed_audio_callback(on_raw_tdm_afe_audio, NULL);
    afe_capture_set_raw_channel_monitor(true);
    app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
    app_ui_set_mic_state("RAW TDM");
    ESP_LOGI(TAG, "raw TDM diag start duration=%dms cap_per_ch=%u", duration_ms, (unsigned)s_raw_tdm_channel_cap);
}

static void upload_raw_tdm_channel(int ch, const char *session_id, uint32_t duration_ms)
{
    if (ch < 0 || ch >= RAW_TDM_DIAG_CHANNELS || !s_raw_tdm_channel_bufs[ch] || s_raw_tdm_channel_lens[ch] == 0) {
        return;
    }
    esp_err_t err = mcp_client_audio_stream_begin(session_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "raw TDM stream begin failed ch=%d err=%s", ch, esp_err_to_name(err));
        return;
    }
    size_t offset = 0;
    while (offset < s_raw_tdm_channel_lens[ch]) {
        size_t chunk = s_raw_tdm_channel_lens[ch] - offset;
        if (chunk > CAPTURE_CHUNK_BYTES) {
            chunk = CAPTURE_CHUNK_BYTES;
        }
        err = mcp_client_audio_stream_chunk(session_id, s_raw_tdm_channel_bufs[ch] + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "raw TDM stream chunk failed ch=%d err=%s offset=%u", ch, esp_err_to_name(err), (unsigned)offset);
            break;
        }
        offset += chunk;
    }

    char reason[32];
    snprintf(reason, sizeof(reason), "raw_tdm_diag_ch%d", ch);
    mcp_client_audio_stream_end(session_id, duration_ms, reason);
}

static void upload_raw_tdm_afe(uint32_t duration_ms)
{
    if (!s_raw_tdm_afe_buf || s_raw_tdm_afe_len == 0) {
        return;
    }

    char session_id[40];
    snprintf(session_id,
             sizeof(session_id),
             "esp32-rawtdm-%lu-afe",
             (unsigned long)s_raw_tdm_start_tick);
    esp_err_t err = mcp_client_audio_stream_begin(session_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "raw TDM AFE stream begin failed err=%s", esp_err_to_name(err));
        return;
    }

    size_t offset = 0;
    while (offset < s_raw_tdm_afe_len) {
        size_t chunk = s_raw_tdm_afe_len - offset;
        if (chunk > CAPTURE_CHUNK_BYTES) {
            chunk = CAPTURE_CHUNK_BYTES;
        }
        err = mcp_client_audio_stream_chunk(session_id, s_raw_tdm_afe_buf + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "raw TDM AFE stream chunk failed err=%s offset=%u", esp_err_to_name(err), (unsigned)offset);
            break;
        }
        offset += chunk;
    }

    mcp_client_audio_stream_end(session_id, duration_ms, "raw_tdm_diag_afe");
}

static void finish_raw_tdm_diag(void)
{
    if (!s_raw_tdm_active && !s_raw_tdm_finish_queued) {
        return;
    }

    afe_capture_set_raw_audio_callback(NULL, NULL);
    afe_capture_set_processed_audio_callback(NULL, NULL);
    afe_capture_set_raw_channel_monitor(false);
    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = s_raw_tdm_start_tick
                               ? (uint32_t)((now - s_raw_tdm_start_tick) * portTICK_PERIOD_MS)
                               : s_raw_tdm_target_ms;
    if (duration_ms == 0 || duration_ms > RAW_TDM_DIAG_MAX_MS + 500) {
        duration_ms = s_raw_tdm_target_ms;
    }
    s_raw_tdm_active = false;
    s_raw_tdm_finish_queued = false;

    ESP_LOGI(TAG,
             "raw TDM diag done duration=%ums feed_channels=%d cap=%u",
             (unsigned)duration_ms,
             s_raw_tdm_channels_seen,
             (unsigned)s_raw_tdm_channel_cap);
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        raw_tdm_channel_stats_t *stats = &s_raw_tdm_stats[ch];
        uint32_t avg = stats->samples ? (uint32_t)(stats->sum_abs / stats->samples) : 0;
        uint32_t rms = stats->samples ? isqrt_u64_local(stats->sum_sq / stats->samples) : 0;
        uint32_t zcr_pm = stats->samples > 1 ? (stats->zero_crossings * 1000) / (stats->samples - 1) : 0;
        ESP_LOGI(TAG,
                 "raw TDM ch%d bytes=%u samples=%u peak=%d avg=%u rms=%u zcr=%u/1000",
                 ch,
                 (unsigned)s_raw_tdm_channel_lens[ch],
                 (unsigned)stats->samples,
                 stats->peak,
                 (unsigned)avg,
                 (unsigned)rms,
                 (unsigned)zcr_pm);
    }
    uint32_t afe_avg = s_raw_tdm_afe_stats.samples ? (uint32_t)(s_raw_tdm_afe_stats.sum_abs / s_raw_tdm_afe_stats.samples) : 0;
    uint32_t afe_rms = s_raw_tdm_afe_stats.samples ? isqrt_u64_local(s_raw_tdm_afe_stats.sum_sq / s_raw_tdm_afe_stats.samples) : 0;
    uint32_t afe_zcr_pm = s_raw_tdm_afe_stats.samples > 1
                              ? (s_raw_tdm_afe_stats.zero_crossings * 1000) / (s_raw_tdm_afe_stats.samples - 1)
                              : 0;
    ESP_LOGI(TAG,
             "raw TDM AFE bytes=%u samples=%u peak=%d avg=%u rms=%u zcr=%u/1000",
             (unsigned)s_raw_tdm_afe_len,
             (unsigned)s_raw_tdm_afe_stats.samples,
             s_raw_tdm_afe_stats.peak,
             (unsigned)afe_avg,
             (unsigned)afe_rms,
             (unsigned)afe_zcr_pm);

    if (!mcp_client_is_connected()) {
        ESP_LOGW(TAG, "raw TDM diag captured but MCP is offline; WAV upload skipped");
        app_ui_set_mcp_status(mcp_client_get_status_text());
        app_ui_set_mic_state("RAW LOGGED");
        raw_tdm_free_buffers();
        raw_tdm_reset_state();
        return;
    }

    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        char session_id[40];
        snprintf(session_id,
                 sizeof(session_id),
                 "esp32-rawtdm-%lu-ch%d",
                 (unsigned long)s_raw_tdm_start_tick,
                 ch);
        upload_raw_tdm_channel(ch, session_id, duration_ms);
    }
    upload_raw_tdm_afe(duration_ms);
    app_ui_set_assistant_state(APP_UI_STATE_IDLE);
    app_ui_set_mic_state("RAW SAVED");
    ESP_LOGI(TAG, "raw TDM diag upload requested; check MCP data/esp32_audio esp32-rawtdm-* files");
    raw_tdm_free_buffers();
    raw_tdm_reset_state();
}

static void raw_tdm_diag_housekeeping(void)
{
    if (!s_raw_tdm_active || s_raw_tdm_finish_queued) {
        return;
    }
    if (xTaskGetTickCount() >= s_raw_tdm_end_tick) {
        queue_raw_tdm_finish();
    }
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
    if (s_audio_busy == busy) {
        return;
    }
    s_audio_busy = busy;
    if (!busy && s_continuous_chat) {
        s_cont_rearm_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CONT_REARM_DELAY_MS);
        cont_vad_reset_runtime();
    }
    ESP_LOGI(TAG, "assistant audio busy=%d continuous=%d", busy, s_continuous_chat);
}

static void on_mcp_assistant_busy(bool busy, void *ctx)
{
    (void)ctx;
    mark_assistant_audio_busy(busy);
}

static bool command_equals(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }
    while (*lhs && *rhs) {
        if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

static bool command_has_token_prefix(const char *command, const char *prefix)
{
    if (!command || !prefix) {
        return false;
    }
    while (*command && *prefix) {
        if (tolower((unsigned char)*command) != tolower((unsigned char)*prefix)) {
            return false;
        }
        ++command;
        ++prefix;
    }
    return *prefix == '\0' && (*command == '\0' || isspace((unsigned char)*command));
}

static void on_mcp_device_command(const char *command, void *ctx)
{
    (void)ctx;
    if (!command || !command[0]) {
        return;
    }
    ESP_LOGI(TAG, "mcp device command: %s", command);
    if (command_equals(command, "chat_toggle") || command_equals(command, "chat")) {
        send_cmd_nonblocking(VOICE_CMD_CHAT_TOGGLE, 0);
        return;
    }
    if (command_equals(command, "chat_start") || command_equals(command, "chat_on") ||
        command_equals(command, "continuous_on")) {
        if (!s_continuous_chat) {
            send_cmd_nonblocking(VOICE_CMD_CHAT_TOGGLE, 0);
        }
        return;
    }
    if (command_equals(command, "chat_stop") || command_equals(command, "chat_off") ||
        command_equals(command, "continuous_off")) {
        if (s_continuous_chat) {
            send_cmd_nonblocking(VOICE_CMD_CHAT_TOGGLE, 0);
        }
        return;
    }
    if (command_equals(command, "wake_toggle")) {
        send_cmd_nonblocking(VOICE_CMD_WAKE_TOGGLE, 0);
        return;
    }
    if (command_equals(command, "wake_on")) {
        if (!s_wake_enabled) {
            send_cmd_nonblocking(VOICE_CMD_WAKE_TOGGLE, 0);
        }
        return;
    }
    if (command_equals(command, "wake_off")) {
        if (s_wake_enabled) {
            send_cmd_nonblocking(VOICE_CMD_WAKE_TOGGLE, 0);
        }
        return;
    }
    if (command_equals(command, "play") || command_equals(command, "xiaole") ||
        command_equals(command, "play_test")) {
        send_cmd_nonblocking(VOICE_CMD_PLAY_ONCE, 0);
        return;
    }
    if (command_has_token_prefix(command, "raw_tdm_diag") ||
        command_has_token_prefix(command, "raw_diag") ||
        command_has_token_prefix(command, "tdm_diag")) {
        const char *space = strchr(command, ' ');
        int duration_ms = space ? atoi(space + 1) : RAW_TDM_DIAG_DEFAULT_MS;
        send_cmd_nonblocking(VOICE_CMD_RAW_TDM_DIAG, duration_ms > 0 ? duration_ms : RAW_TDM_DIAG_DEFAULT_MS);
        return;
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
        } else if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(CONT_VAD_WATCHDOG_MS)) != pdTRUE) {
            raw_tdm_diag_housekeeping();
            continuous_chat_housekeeping();
            continue;
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
                if (s_afe_ready) {
                    app_ui_set_mic_state("AFE READY");
                } else if (ensure_mic_diag_ready()) {
                    mic_diag_start();
                    app_ui_set_mic_state("MIC ON");
                } else {
                    app_ui_set_mic_state("MIC ERROR");
                }
                break;
            case VOICE_CMD_MIC_OFF:
                if (!s_afe_ready && s_mic_diag_ready) {
                    mic_diag_stop();
                }
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
                if (s_wake_enabled) {
                    ensure_afe_ready();
                }
                app_ui_set_wake_enabled(s_wake_enabled);
                if (s_afe_ready) {
                    afe_capture_set_wake_enabled(s_wake_enabled);
                }
                app_ui_set_voice_state(s_wake_enabled
                                           ? (s_afe_ready && afe_capture_has_wake_model() ? "WAKE ON" : "WAKE TODO")
                                           : "VOICE READY");
                send_runtime_config();
                ESP_LOGI(TAG,
                         "wake word UI flag=%d afe_ready=%d wake_model=%d",
                         s_wake_enabled,
                         s_afe_ready,
                         s_afe_ready ? afe_capture_has_wake_model() : 0);
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
            case VOICE_CMD_RAW_TDM_DIAG:
                start_raw_tdm_diag(cmd.value > 0 ? cmd.value : RAW_TDM_DIAG_DEFAULT_MS);
                break;
            case VOICE_CMD_RAW_TDM_DONE:
                finish_raw_tdm_diag();
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
        } else if (strcmp(line, "RAW TDM") == 0 || strcmp(line, "RAW") == 0 || strcmp(line, "TDM") == 0) {
            send_cmd(VOICE_CMD_RAW_TDM_DIAG, RAW_TDM_DIAG_DEFAULT_MS);
        } else if (strncmp(line, "RAW TDM ", 8) == 0) {
            send_cmd(VOICE_CMD_RAW_TDM_DIAG, atoi(line + 8));
        } else if (strncmp(line, "RAW ", 4) == 0) {
            send_cmd(VOICE_CMD_RAW_TDM_DIAG, atoi(line + 4));
        } else if (strncmp(line, "TDM ", 4) == 0) {
            send_cmd(VOICE_CMD_RAW_TDM_DIAG, atoi(line + 4));
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
    esp_log_level_set("AFE_CAPTURE", ESP_LOG_INFO);
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

    mcp_client_init();
    mcp_client_set_busy_callback(on_mcp_assistant_busy, NULL);
    mcp_client_set_device_command_callback(on_mcp_device_command, NULL);
    app_ui_set_mcp_status(mcp_client_get_status_text());

    s_cmd_queue = xQueueCreate(16, sizeof(voice_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "command queue create failed");
        return;
    }

    if (mcp_client_connect() == ESP_OK) {
        app_ui_set_mcp_status(mcp_client_get_status_text());
        vTaskDelay(pdMS_TO_TICKS(900));
    }

    app_ui_set_mic_state("MIC READY");
    ESP_LOGI(TAG,
             "full duplex AFE/AEC experimental path=%d lazy_ready=%d; capture path=%s",
             ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL,
             s_afe_ready,
             ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL ? "AFE/AEC lazy" : "mono PCM");
    send_runtime_config();

    xTaskCreate(playback_task, "voice_playback", 4096, NULL, 5, NULL);
    xTaskCreate(command_task, "voice_command", 4096, NULL, 4, NULL);

    if (app_buttons_init(on_button_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button init failed");
    }

    if (!mcp_client_is_connected()) {
        send_cmd(VOICE_CMD_MCP_CONNECT, 0);
    }
}
