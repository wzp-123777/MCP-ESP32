#include <ctype.h>
#include <math.h>
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
#define CONT_STARTUP_REARM_MS 1200
#define CONT_REARM_DELAY_MS 1200
#define CONT_PLAYBACK_TAIL_IGNORE_MS 900
#define CONT_PLAYBACK_TAIL_REJECT_MS 600
#define CONT_PLAYBACK_TAIL_GATE_MS 1200
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
#define CONT_RNNM_AFE_VAD_CONFIRM_AVG 300
#define CONT_RNNM_AFE_VAD_CONFIRM_PEAK 1400
#define CONT_RNNM_AFE_VAD_CONFIRM_MARGIN 150
#define CONT_RNNM_AFE_VAD_CONFIRM_STRONG_AVG 520
#define CONT_RNNM_AFE_VAD_CONFIRM_STRONG_PEAK 3500
#define CONT_RNNM_AFE_VAD_CONFIRM_STRONG_MARGIN 260
#define CONT_RNNM_AFE_VAD_CONFIRM_HITS 6
#define CONT_RNNM_AFE_LOCAL_START_AVG 380
#define CONT_RNNM_AFE_LOCAL_START_PEAK 2200
#define CONT_RNNM_AFE_LOCAL_START_MARGIN 220
#define CONT_RNNM_AFE_LOCAL_START_HITS 6
#define CONT_AFE_REJECT_REARM_MS 1200
#define CONT_BARGE_REF_CH 0
#define CONT_BARGE_MIC_CH 3
#define CONT_BARGE_RAW_CORR_WINDOW 8
#define CONT_BARGE_RAW_STALE_MS 180
#define CONT_BARGE_CANDIDATE_MIN_MS 200
#define CONT_BARGE_CANDIDATE_MAX_MS 380
#define CONT_BARGE_ACCEPT_HITS 3
#define CONT_BARGE_ECHO_REJECT_HITS 3
#define CONT_BARGE_REF_ACTIVE_AVG 120
#define CONT_BARGE_REF_ACTIVE_PEAK 900
#define CONT_BARGE_CORR_HIGH 650
#define CONT_BARGE_CORR_LOW 450
#define CONT_BARGE_ECHO_GAIN_INIT 2200
#define CONT_BARGE_ECHO_GAIN_MIN 600
#define CONT_BARGE_ECHO_GAIN_MAX 5000
#define CONT_BARGE_MIC_EXCESS_AVG 260
#define CONT_BARGE_MIC_EXCESS_PEAK 1200
#define CONT_BARGE_LOCAL_MIC_AVG 220
#define CONT_BARGE_LOCAL_MIC_PEAK 700
#define CONT_BARGE_OVERRIDE_MIC_AVG 480
#define CONT_BARGE_OVERRIDE_MIC_PEAK 1500
#define CONT_BARGE_OVERRIDE_CORR_MAX 820
#define CONT_BARGE_STRONG_EXTRA_AVG 180
#define CONT_BARGE_STRONG_EXTRA_PEAK 900
#define CONT_PREROLL_CHUNKS 16
#define RAW_TDM_DIAG_CHANNELS 4
#define RAW_TDM_DIAG_RATE 16000
#define RAW_TDM_DIAG_DEFAULT_MS 3000
#define RAW_TDM_DIAG_MIN_MS 500
#define RAW_TDM_DIAG_MAX_MS 5000
#define BARGE_DIAG_DEFAULT_MS 5000
#define BARGE_DIAG_MIN_MS 1500
#define BARGE_DIAG_MAX_MS 12000
#define BARGE_DIAG_LOG_INTERVAL_MS 700
#define BARGE_DIAG_PLAYBACK_TAIL_MS 900
#define BARGE_REF_ENV_BLOCK_FRAMES 512
#define BARGE_REF_ENV_MAX_BLOCKS 512
#define BARGE_REF_ENV_MAX_LAG_BLOCKS 96
#define BARGE_REF_ENV_MIN_OVERLAP_BLOCKS 8
#define BARGE_DIAG_VALUE_DURATION_MASK 0xFFFF
#define BARGE_DIAG_VALUE_FMT_SHIFT 16
#define BARGE_DIAG_VALUE_FMT_MASK 0xF
#define BARGE_DIAG_VALUE_PROMPT_SHIFT 20
#define BARGE_DIAG_VALUE_PROMPT_MASK 0xF
#define BARGE_DIAG_AFTER_DEFAULT_MS 9000
#define BARGE_DIAG_INTERRUPT_DEFAULT_MS 12000
#define BARGE_DIAG_INTERRUPT_REPEATS 3

typedef enum {
    CAPTURE_MODE_NONE = 0,
    CAPTURE_MODE_PTT,
    CAPTURE_MODE_CONTINUOUS,
} capture_mode_t;

typedef enum {
    BARGE_DIAG_PROMPT_XIAOLE = 0,
    BARGE_DIAG_PROMPT_AFTER,
    BARGE_DIAG_PROMPT_INTERRUPT,
    BARGE_DIAG_PROMPT_COUNT,
} barge_diag_prompt_t;

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
    VOICE_CMD_MCP_ENDPOINT_DEFAULT,
    VOICE_CMD_SET_PRESS,
    VOICE_CMD_SET_RELEASE,
    VOICE_CMD_CHAT_TOGGLE,
    VOICE_CMD_WAKE_TOGGLE,
    VOICE_CMD_PERSONA_NEXT,
    VOICE_CMD_VOICE_NEXT,
    VOICE_CMD_RAW_TDM_DIAG,
    VOICE_CMD_RAW_TDM_DONE,
    VOICE_CMD_BARGE_DIAG,
    VOICE_CMD_BARGE_DIAG_DONE,
    VOICE_CMD_AFE_VAD_MUTE_SET,
    VOICE_CMD_AFE_AEC_PROFILE_SET,
    VOICE_CMD_AFE_STATUS,
} voice_cmd_type_t;

typedef enum {
    CONT_BARGE_DECISION_PENDING = 0,
    CONT_BARGE_DECISION_ACCEPT,
    CONT_BARGE_DECISION_REJECT,
} cont_barge_decision_t;

typedef enum {
    CONT_AFE_SUPPRESS_NONE = 0,
    CONT_AFE_SUPPRESS_REARM,
    CONT_AFE_SUPPRESS_REMOTE_BUSY,
    CONT_AFE_SUPPRESS_CONFIRM_TIMEOUT,
    CONT_AFE_SUPPRESS_BARGE_REJECT,
} cont_afe_suppress_reason_t;

typedef struct {
    voice_cmd_type_t type;
    int value;
} voice_cmd_t;

typedef struct {
    const char *fmt;
    const char *label;
} afe_diag_format_t;

typedef struct {
    int peak;
    int64_t sum_abs;
    uint64_t sum_sq;
    uint32_t samples;
    uint32_t zero_crossings;
    int16_t prev;
    bool prev_valid;
} raw_tdm_channel_stats_t;

typedef struct {
    uint16_t *blocks;
    uint32_t count;
    uint32_t current_frames;
    uint64_t current_sum_abs;
} ref_env_track_t;

typedef struct {
    int channel;
    int score_permille;
    int lag_blocks;
    int lag_ms;
    int gain_permille;
    uint32_t ref_blocks;
    uint32_t raw_blocks;
} ref_corr_result_t;

typedef struct {
    TickType_t tick;
    int frames;
    int channels;
    int ref_avg;
    int ref_peak;
    int mic_avg;
    int mic_peak;
    int corr_permille;
} cont_barge_raw_snapshot_t;

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
static bool s_assistant_playback_busy;
static volatile bool s_loop_enabled;
static volatile bool s_barge_playback_cancelled;
static TickType_t s_cont_speech_start_tick;
static TickType_t s_cont_last_voice_tick;
static TickType_t s_cont_last_audio_tick;
static TickType_t s_cont_rearm_tick;
static TickType_t s_cont_playback_tail_until_tick;
static TickType_t s_cont_playback_gate_until_tick;
static TickType_t s_cont_vad_log_tick;
static TickType_t s_cont_busy_log_tick;
static TickType_t s_cont_tail_log_tick;
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
static bool s_afe_vad_suppressed;
static TickType_t s_afe_vad_suppressed_tick;
static cont_afe_suppress_reason_t s_afe_vad_suppress_reason;
static bool s_cont_barge_candidate_active;
static bool s_cont_barge_candidate_playback;
static bool s_cont_barge_candidate_tail;
static TickType_t s_cont_barge_candidate_start_tick;
static TickType_t s_cont_barge_candidate_log_tick;
static int s_cont_barge_accept_hits;
static int s_cont_barge_echo_hits;
static bool s_cont_barge_start_local_override;
static int s_cont_barge_max_afe_avg;
static int s_cont_barge_max_afe_peak;
static int s_cont_barge_max_ref_avg;
static int s_cont_barge_max_mic_avg;
static int s_cont_barge_echo_gain_permille = CONT_BARGE_ECHO_GAIN_INIT;
static cont_barge_raw_snapshot_t s_cont_barge_raw;
static uint16_t s_cont_barge_ref_env[CONT_BARGE_RAW_CORR_WINDOW];
static uint16_t s_cont_barge_mic_env[CONT_BARGE_RAW_CORR_WINDOW];
static int s_cont_barge_env_pos;
static int s_cont_barge_env_count;
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
static volatile bool s_barge_diag_active;
static volatile bool s_barge_diag_finish_queued;
static volatile bool s_barge_diag_playing;
static TickType_t s_barge_diag_start_tick;
static TickType_t s_barge_diag_end_tick;
static TickType_t s_barge_diag_play_start_tick;
static TickType_t s_barge_diag_play_end_tick;
static TickType_t s_barge_diag_log_tick;
static uint32_t s_barge_diag_target_ms;
static uint32_t s_barge_diag_vad_start_count;
static uint32_t s_barge_diag_vad_end_count;
static uint32_t s_barge_diag_vad_start_play_count;
static uint32_t s_barge_diag_vad_start_tail_count;
static uint32_t s_barge_diag_vad_start_post_count;
static uint32_t s_barge_diag_wake_count;
static int s_barge_diag_channels_seen;
static int s_barge_diag_prompt_id = BARGE_DIAG_PROMPT_XIAOLE;
static TickType_t s_barge_diag_play_tail_end_tick;
static raw_tdm_channel_stats_t s_barge_diag_raw_play_stats[RAW_TDM_DIAG_CHANNELS];
static raw_tdm_channel_stats_t s_barge_diag_raw_post_stats[RAW_TDM_DIAG_CHANNELS];
static raw_tdm_channel_stats_t s_barge_diag_afe_play_stats;
static raw_tdm_channel_stats_t s_barge_diag_afe_post_stats;
static ref_env_track_t s_barge_ref_env;
static ref_env_track_t s_barge_raw_env[RAW_TDM_DIAG_CHANNELS];
static ref_corr_result_t s_barge_ref_corr;

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

static const afe_diag_format_t s_afe_diag_formats[] = {
    {"RNNM", "ch0=ref ch1=null ch2=null ch3=mic"},
    {"RMNM", "ch0=ref ch1=mic ch2=null ch3=mic"},
    {"RMNN", "ch0=ref ch1=mic ch2=null ch3=null"},
    {"MRNN", "ch0=mic ch1=ref ch2=null ch3=null"},
    {"RMMN", "ch0=ref ch1=mic ch2=mic ch3=null"},
};

static size_t s_persona_index;
static size_t s_voice_index;

static void make_capture_session_id(void);
static void finish_continuous_utterance(const char *reason);
static void clear_afe_vad_pending(void);
static void cont_afe_suppress_segment(TickType_t now, cont_afe_suppress_reason_t reason);
static void cont_afe_clear_suppressed(void);
static void extend_afe_reject_until(TickType_t until_tick);
static void mark_assistant_playback_busy(bool busy);
static void suppress_playback_for_barge(void);
static void on_mcp_playback_busy(bool busy, void *ctx);
static void cont_barge_begin_candidate(TickType_t now, bool playback, bool tail);
static void cont_barge_reset_candidate(void);
static void cont_preroll_reset(void);
static bool cont_playback_tail_gate_active(TickType_t now);
static cont_barge_decision_t cont_barge_process_candidate(TickType_t now,
                                                          int avg_abs,
                                                          int peak,
                                                          vad_state_t sr_vad_state);
static bool cont_barge_tail_probe_hit(TickType_t now, int avg_abs, int peak, vad_state_t sr_vad_state);
static bool cont_barge_accept_candidate(TickType_t now,
                                        int avg_abs,
                                        int peak,
                                        vad_state_t sr_vad_state);
static void on_raw_tdm_audio(const int16_t *interleaved, int frames, int channels, void *ctx);
static void on_mcp_device_command(const char *command, void *ctx);
static void start_continuous_chat(void);
static void stop_continuous_chat(void);
static void start_raw_tdm_diag(int duration_ms);
static void finish_raw_tdm_diag(void);
static void raw_tdm_diag_housekeeping(void);
static void start_barge_diag(int duration_ms);
static void finish_barge_diag(void);
static void barge_diag_housekeeping(void);
static int clamp_int(int value, int low, int high);
static int max_int(int a, int b);
static int parse_barge_diag_value(const char *args);
static int parse_on_off_value(const char *text);
static int parse_afe_aec_profile_value(const char *text);
static void log_afe_status(void);
static bool apply_afe_vad_mute_playback(bool enabled);
static bool apply_afe_aec_profile(afe_capture_aec_profile_t profile);

static void print_help(void)
{
    ESP_LOGI(TAG, "commands: ASK <text>, REC <ms>, CHAT, WAKE, RAW TDM [ms], BARGE [fmt] [xiaole|after|interrupt] [ms], AFE STATUS, AFE AEC MODE LOW/HIGH, AFE VAD MUTE ON/OFF, PERSONA, VOICE, PLAY/XIAOLE, LOOP, STOP, MIC ON, MIC OFF, VOL 0-100, VOL+, VOL-, MCP URL <url>|DEFAULT, MCP CONNECT, HELP");
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

static int barge_diag_default_ms_for_prompt(int prompt_id)
{
    switch (prompt_id) {
        case BARGE_DIAG_PROMPT_AFTER:
            return BARGE_DIAG_AFTER_DEFAULT_MS;
        case BARGE_DIAG_PROMPT_INTERRUPT:
            return BARGE_DIAG_INTERRUPT_DEFAULT_MS;
        case BARGE_DIAG_PROMPT_XIAOLE:
        default:
            return BARGE_DIAG_DEFAULT_MS;
    }
}

static int normalize_barge_diag_prompt_id(int prompt_id)
{
    if (prompt_id < 0 || prompt_id >= BARGE_DIAG_PROMPT_COUNT) {
        return BARGE_DIAG_PROMPT_XIAOLE;
    }
    return prompt_id;
}

static const char *barge_diag_prompt_name(int prompt_id)
{
    switch (normalize_barge_diag_prompt_id(prompt_id)) {
        case BARGE_DIAG_PROMPT_AFTER:
            return "after";
        case BARGE_DIAG_PROMPT_INTERRUPT:
            return "interrupt";
        case BARGE_DIAG_PROMPT_XIAOLE:
        default:
            return "xiaole";
    }
}

static bool token_equals_ignore_case(const char *token, size_t len, const char *expected)
{
    if (!token || !expected || strlen(expected) != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)token[i]) != tolower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

static int barge_diag_prompt_id_from_token(const char *token, size_t len)
{
    if (token_equals_ignore_case(token, len, "xiaole") ||
        token_equals_ignore_case(token, len, "probe") ||
        token_equals_ignore_case(token, len, "round1") ||
        token_equals_ignore_case(token, len, "r1")) {
        return BARGE_DIAG_PROMPT_XIAOLE;
    }
    if (token_equals_ignore_case(token, len, "after") ||
        token_equals_ignore_case(token, len, "answer") ||
        token_equals_ignore_case(token, len, "round2") ||
        token_equals_ignore_case(token, len, "r2")) {
        return BARGE_DIAG_PROMPT_AFTER;
    }
    if (token_equals_ignore_case(token, len, "interrupt") ||
        token_equals_ignore_case(token, len, "bargein") ||
        token_equals_ignore_case(token, len, "barge-in") ||
        token_equals_ignore_case(token, len, "round3") ||
        token_equals_ignore_case(token, len, "r3")) {
        return BARGE_DIAG_PROMPT_INTERRUPT;
    }
    return -1;
}

static int make_barge_diag_value(int duration_ms, int fmt_id, int prompt_id)
{
    prompt_id = normalize_barge_diag_prompt_id(prompt_id);
    if (duration_ms <= 0) {
        duration_ms = barge_diag_default_ms_for_prompt(prompt_id);
    }
    duration_ms = clamp_int(duration_ms, BARGE_DIAG_MIN_MS, BARGE_DIAG_MAX_MS);
    if (fmt_id < 0 || fmt_id > (int)(sizeof(s_afe_diag_formats) / sizeof(s_afe_diag_formats[0]))) {
        fmt_id = 0;
    }
    return ((prompt_id & BARGE_DIAG_VALUE_PROMPT_MASK) << BARGE_DIAG_VALUE_PROMPT_SHIFT) |
           ((fmt_id & BARGE_DIAG_VALUE_FMT_MASK) << BARGE_DIAG_VALUE_FMT_SHIFT) |
           (duration_ms & BARGE_DIAG_VALUE_DURATION_MASK);
}

static int barge_diag_value_duration(int value)
{
    int duration_ms = value & BARGE_DIAG_VALUE_DURATION_MASK;
    if (duration_ms <= 0) {
        duration_ms = value > 0 && value <= BARGE_DIAG_MAX_MS
                          ? value
                          : barge_diag_default_ms_for_prompt((value >> BARGE_DIAG_VALUE_PROMPT_SHIFT) & BARGE_DIAG_VALUE_PROMPT_MASK);
    }
    return clamp_int(duration_ms, BARGE_DIAG_MIN_MS, BARGE_DIAG_MAX_MS);
}

static int barge_diag_value_fmt_id(int value)
{
    int fmt_id = (value >> BARGE_DIAG_VALUE_FMT_SHIFT) & BARGE_DIAG_VALUE_FMT_MASK;
    if (fmt_id < 0 || fmt_id > (int)(sizeof(s_afe_diag_formats) / sizeof(s_afe_diag_formats[0]))) {
        return 0;
    }
    return fmt_id;
}

static int barge_diag_value_prompt_id(int value)
{
    return normalize_barge_diag_prompt_id((value >> BARGE_DIAG_VALUE_PROMPT_SHIFT) & BARGE_DIAG_VALUE_PROMPT_MASK);
}

static const char *barge_diag_fmt_from_id(int fmt_id)
{
    if (fmt_id <= 0 || fmt_id > (int)(sizeof(s_afe_diag_formats) / sizeof(s_afe_diag_formats[0]))) {
        return NULL;
    }
    return s_afe_diag_formats[fmt_id - 1].fmt;
}

static int barge_diag_fmt_id_from_token(const char *token, size_t len)
{
    if (!token || len == 0 || len > 7) {
        return 0;
    }
    char fmt[8];
    for (size_t i = 0; i < len; ++i) {
        fmt[i] = (char)toupper((unsigned char)token[i]);
    }
    fmt[len] = '\0';
    for (size_t i = 0; i < sizeof(s_afe_diag_formats) / sizeof(s_afe_diag_formats[0]); ++i) {
        if (strcmp(fmt, s_afe_diag_formats[i].fmt) == 0) {
            return (int)i + 1;
        }
    }
    return 0;
}

static int parse_barge_diag_value(const char *args)
{
    int fmt_id = 0;
    int prompt_id = BARGE_DIAG_PROMPT_XIAOLE;
    int duration_ms = 0;
    const char *p = args;
    while (p && *p) {
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char *token = p;
        while (*p && !isspace((unsigned char)*p)) {
            ++p;
        }
        size_t token_len = (size_t)(p - token);
        if (token_len > 0 && isdigit((unsigned char)token[0])) {
            duration_ms = atoi(token);
        } else {
            int parsed_fmt_id = barge_diag_fmt_id_from_token(token, token_len);
            if (parsed_fmt_id != 0) {
                fmt_id = parsed_fmt_id;
                continue;
            }
            int parsed_prompt_id = barge_diag_prompt_id_from_token(token, token_len);
            if (parsed_prompt_id >= 0) {
                prompt_id = parsed_prompt_id;
                continue;
            }
            ESP_LOGW(TAG, "unknown barge diag token: %.*s", (int)token_len, token);
        }
    }
    return make_barge_diag_value(duration_ms, fmt_id, prompt_id);
}

static int parse_on_off_value(const char *text)
{
    if (!text) {
        return -1;
    }
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    const char *token = text;
    while (*text && !isspace((unsigned char)*text)) {
        ++text;
    }
    size_t len = (size_t)(text - token);
    if (token_equals_ignore_case(token, len, "on") ||
        token_equals_ignore_case(token, len, "true") ||
        token_equals_ignore_case(token, len, "1") ||
        token_equals_ignore_case(token, len, "yes")) {
        return 1;
    }
    if (token_equals_ignore_case(token, len, "off") ||
        token_equals_ignore_case(token, len, "false") ||
        token_equals_ignore_case(token, len, "0") ||
        token_equals_ignore_case(token, len, "no")) {
        return 0;
    }
    return -1;
}

static int parse_afe_aec_profile_value(const char *text)
{
    if (!text) {
        return -1;
    }
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    const char *token = text;
    while (*text && !isspace((unsigned char)*text)) {
        ++text;
    }
    size_t len = (size_t)(text - token);
    if (token_equals_ignore_case(token, len, "low") ||
        token_equals_ignore_case(token, len, "low_cost") ||
        token_equals_ignore_case(token, len, "fd_low") ||
        token_equals_ignore_case(token, len, "fd_low_cost") ||
        token_equals_ignore_case(token, len, "lc")) {
        return AFE_CAPTURE_AEC_PROFILE_FD_LOW_COST;
    }
    if (token_equals_ignore_case(token, len, "high") ||
        token_equals_ignore_case(token, len, "high_perf") ||
        token_equals_ignore_case(token, len, "fd_high") ||
        token_equals_ignore_case(token, len, "fd_high_perf") ||
        token_equals_ignore_case(token, len, "perf") ||
        token_equals_ignore_case(token, len, "hp")) {
        return AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF;
    }
    return -1;
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

static void barge_diag_note_afe_event(afe_capture_event_t event)
{
    if (!s_barge_diag_active) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    bool in_tail = !s_barge_diag_playing &&
                   s_barge_diag_play_tail_end_tick &&
                   now < s_barge_diag_play_tail_end_tick;
    const char *phase = s_barge_diag_playing ? "playback" : (in_tail ? "playback_tail" : "post");
    switch (event) {
        case AFE_CAPTURE_EVENT_VAD_START:
            ++s_barge_diag_vad_start_count;
            if (s_barge_diag_playing) {
                ++s_barge_diag_vad_start_play_count;
            } else if (in_tail) {
                ++s_barge_diag_vad_start_tail_count;
            } else {
                ++s_barge_diag_vad_start_post_count;
            }
            ESP_LOGW(TAG,
                     "barge diag AFE VAD start phase=%s total=%u play=%u tail=%u post=%u",
                     phase,
                     (unsigned)s_barge_diag_vad_start_count,
                     (unsigned)s_barge_diag_vad_start_play_count,
                     (unsigned)s_barge_diag_vad_start_tail_count,
                     (unsigned)s_barge_diag_vad_start_post_count);
            break;
        case AFE_CAPTURE_EVENT_VAD_END:
            ++s_barge_diag_vad_end_count;
            ESP_LOGI(TAG, "barge diag AFE VAD end phase=%s total=%u", phase, (unsigned)s_barge_diag_vad_end_count);
            break;
        case AFE_CAPTURE_EVENT_WAKE:
            ++s_barge_diag_wake_count;
            ESP_LOGW(TAG, "barge diag WakeNet trigger phase=%s total=%u", phase, (unsigned)s_barge_diag_wake_count);
            break;
        case AFE_CAPTURE_EVENT_ERROR:
            ESP_LOGW(TAG, "barge diag AFE error event");
            break;
    }
}

static void on_afe_event(afe_capture_event_t event, void *ctx)
{
    (void)ctx;
    if (s_barge_diag_active) {
        barge_diag_note_afe_event(event);
        if (event != AFE_CAPTURE_EVENT_ERROR) {
            return;
        }
    }
    switch (event) {
        case AFE_CAPTURE_EVENT_VAD_START:
            ESP_LOGI(TAG, "afe vad start");
            if (s_capture_mode == CAPTURE_MODE_CONTINUOUS &&
                s_continuous_chat &&
                !s_continuous_speaking &&
                mcp_client_is_connected()) {
                TickType_t now = xTaskGetTickCount();
                bool playback_busy = s_assistant_playback_busy;
                bool assistant_busy = s_audio_busy || mcp_client_is_assistant_busy();
                bool playback_tail = cont_playback_tail_gate_active(now);
                if (playback_busy || playback_tail) {
                    cont_barge_begin_candidate(now, playback_busy, playback_tail);
                    return;
                }
                if (assistant_busy) {
                    cont_afe_suppress_segment(now, CONT_AFE_SUPPRESS_REMOTE_BUSY);
                    return;
                }
                if (now < s_cont_rearm_tick || now < s_afe_vad_reject_until_tick) {
                    cont_afe_suppress_segment(now, CONT_AFE_SUPPRESS_REARM);
                    return;
                }
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
            } else if (s_cont_barge_candidate_active) {
                ESP_LOGI(TAG, "barge candidate dropped before accept on afe vad end");
                extend_afe_reject_until(xTaskGetTickCount() + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS));
                cont_barge_reset_candidate();
                cont_preroll_reset();
            } else if (s_afe_vad_suppressed) {
                TickType_t now = xTaskGetTickCount();
                cont_afe_clear_suppressed();
                extend_afe_reject_until(now + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS));
            } else if (s_afe_vad_pending) {
                ESP_LOGI(TAG, "afe vad dropped before energy confirm");
                extend_afe_reject_until(xTaskGetTickCount() + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS));
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
        ESP_LOGI(TAG,
                 "AFE/AEC ready; capture path=AFE/AEC input=%s aec_profile=%s vad_mute=%d",
                 afe_capture_get_input_format(),
                 afe_capture_get_aec_profile_name(),
                 afe_capture_get_vad_mute_playback());
    }
    return s_afe_ready;
}

static void log_afe_status(void)
{
    ESP_LOGI(TAG,
             "AFE status ready=%d init_attempted=%d running=%d input=%s aec_profile=%s vad_mute=%d wake_enabled=%d wake_model=%d",
             s_afe_ready,
             s_afe_init_attempted,
             afe_capture_is_running(),
             afe_capture_get_input_format(),
             afe_capture_get_aec_profile_name(),
             afe_capture_get_vad_mute_playback(),
             s_wake_enabled,
             s_afe_ready ? afe_capture_has_wake_model() : 0);
}

static bool apply_afe_diag_format(const char *fmt)
{
    if (!fmt || !fmt[0]) {
        return true;
    }
    if (s_afe_ready && afe_capture_is_running()) {
        ESP_LOGW(TAG, "cannot switch AFE format while capture is running");
        return false;
    }

    bool changed = strcmp(afe_capture_get_input_format(), fmt) != 0;
    esp_err_t err = afe_capture_set_input_format(fmt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AFE input format switch failed fmt=%s err=%s", fmt, esp_err_to_name(err));
        return false;
    }
    if (changed) {
        s_afe_ready = false;
        s_afe_init_attempted = false;
        mcp_client_set_sr_enabled(false);
    }
    return ensure_afe_ready();
}

static bool apply_afe_vad_mute_playback(bool enabled)
{
    bool restart_continuous = s_continuous_chat &&
                              s_capture_mode == CAPTURE_MODE_CONTINUOUS &&
                              !s_continuous_speaking;
    if (s_afe_ready && afe_capture_is_running()) {
        if (!restart_continuous) {
            ESP_LOGW(TAG, "cannot switch vad_mute_playback while capture is running");
            return false;
        }
        ESP_LOGI(TAG, "restart continuous chat to switch vad_mute_playback=%d", enabled);
        stop_continuous_chat();
    }

    bool changed = afe_capture_get_vad_mute_playback() != enabled;
    esp_err_t err = afe_capture_set_vad_mute_playback(enabled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "vad_mute_playback switch failed enabled=%d err=%s", enabled, esp_err_to_name(err));
        return false;
    }
    if (changed) {
        s_afe_ready = false;
        s_afe_init_attempted = false;
        mcp_client_set_sr_enabled(false);
    }
    bool ready = ensure_afe_ready();
    app_ui_set_voice_state(enabled ? "VAD MUTE ON" : "VAD MUTE OFF");
    ESP_LOGI(TAG, "vad_mute_playback=%d ready=%d", enabled, ready);
    if (restart_continuous && ready) {
        start_continuous_chat();
    }
    return ready;
}

static bool apply_afe_aec_profile(afe_capture_aec_profile_t profile)
{
    bool restart_continuous = s_continuous_chat &&
                              s_capture_mode == CAPTURE_MODE_CONTINUOUS &&
                              !s_continuous_speaking;
    if (s_afe_ready && afe_capture_is_running()) {
        if (!restart_continuous) {
            ESP_LOGW(TAG, "cannot switch AEC profile while capture is running");
            return false;
        }
        ESP_LOGI(TAG, "restart continuous chat to switch AEC profile");
        stop_continuous_chat();
    }

    bool changed = afe_capture_get_aec_profile() != profile;
    esp_err_t err = afe_capture_set_aec_profile(profile);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AEC profile switch failed profile=%d err=%s", profile, esp_err_to_name(err));
        return false;
    }
    if (changed) {
        s_afe_ready = false;
        s_afe_init_attempted = false;
        mcp_client_set_sr_enabled(false);
    }
    bool ready = ensure_afe_ready();
    app_ui_set_voice_state(profile == AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF ? "AEC HIGH" : "AEC LOW");
    ESP_LOGI(TAG, "AEC profile=%s ready=%d", afe_capture_get_aec_profile_name(), ready);
    if (restart_continuous && ready) {
        start_continuous_chat();
    }
    return ready;
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

static int compute_env_pair_corr_permille(const uint16_t *ref, const uint16_t *mic, int count)
{
    if (!ref || !mic || count < 4) {
        return 0;
    }

    double sum_ref = 0.0;
    double sum_mic = 0.0;
    double sum_ref2 = 0.0;
    double sum_mic2 = 0.0;
    double sum_cross = 0.0;
    for (int i = 0; i < count; ++i) {
        double x = (double)ref[i];
        double y = (double)mic[i];
        sum_ref += x;
        sum_mic += y;
        sum_ref2 += x * x;
        sum_mic2 += y * y;
        sum_cross += x * y;
    }

    double var_ref = (double)count * sum_ref2 - sum_ref * sum_ref;
    double var_mic = (double)count * sum_mic2 - sum_mic * sum_mic;
    if (var_ref <= 1.0 || var_mic <= 1.0) {
        return 0;
    }

    double corr = ((double)count * sum_cross - sum_ref * sum_mic) / sqrt(var_ref * var_mic);
    if (corr < 0.0) {
        corr = -corr;
    }
    if (corr > 1.0) {
        corr = 1.0;
    }
    return (int)(corr * 1000.0 + 0.5);
}

static bool cont_using_aec_path(void)
{
    return s_afe_ready && s_capture_mode == CAPTURE_MODE_CONTINUOUS;
}

static bool cont_using_rnnm_path(void)
{
    const char *fmt = afe_capture_get_input_format();
    return cont_using_aec_path() && fmt && strcmp(fmt, "RNNM") == 0;
}

static void extend_afe_reject_until(TickType_t until_tick)
{
    if (until_tick > s_afe_vad_reject_until_tick) {
        s_afe_vad_reject_until_tick = until_tick;
    }
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
    s_cont_tail_log_tick = 0;
    s_cont_utterance_peak_avg = 0;
    s_cont_last_audio_tick = 0;
    clear_afe_vad_pending();
    cont_barge_reset_candidate();
    s_afe_vad_reject_until_tick = 0;
    s_afe_vad_suppressed = false;
    s_afe_vad_suppressed_tick = 0;
    s_afe_vad_suppress_reason = CONT_AFE_SUPPRESS_NONE;
    cont_sr_vad_reset();
}

static void clear_afe_vad_pending(void)
{
    s_afe_vad_pending = false;
    s_afe_vad_pending_tick = 0;
    s_afe_vad_confirm_hits = 0;
}

static const char *cont_afe_suppress_reason_name(cont_afe_suppress_reason_t reason)
{
    switch (reason) {
        case CONT_AFE_SUPPRESS_REARM:
            return "rearm";
        case CONT_AFE_SUPPRESS_REMOTE_BUSY:
            return "remote_busy";
        case CONT_AFE_SUPPRESS_CONFIRM_TIMEOUT:
            return "confirm_timeout";
        case CONT_AFE_SUPPRESS_BARGE_REJECT:
            return "barge_reject";
        case CONT_AFE_SUPPRESS_NONE:
        default:
            return "none";
    }
}

static void cont_afe_suppress_segment(TickType_t now, cont_afe_suppress_reason_t reason)
{
    clear_afe_vad_pending();
    s_cont_start_hits = 0;
    cont_preroll_reset();
    if (!s_afe_vad_suppressed || s_afe_vad_suppress_reason != reason) {
        ESP_LOGI(TAG,
                 "afe vad segment suppressed reason=%s rearm_left=%dms reject_left=%dms",
                 cont_afe_suppress_reason_name(reason),
                 now < s_cont_rearm_tick ? (int)((s_cont_rearm_tick - now) * portTICK_PERIOD_MS) : 0,
                 now < s_afe_vad_reject_until_tick ? (int)((s_afe_vad_reject_until_tick - now) * portTICK_PERIOD_MS) : 0);
    }
    s_afe_vad_suppressed = true;
    s_afe_vad_suppressed_tick = now;
    s_afe_vad_suppress_reason = reason;
}

static void cont_afe_clear_suppressed(void)
{
    if (!s_afe_vad_suppressed) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = s_afe_vad_suppressed_tick
                               ? (uint32_t)((now - s_afe_vad_suppressed_tick) * portTICK_PERIOD_MS)
                               : 0;
    ESP_LOGI(TAG,
             "afe vad suppressed segment ended reason=%s duration=%ums",
             cont_afe_suppress_reason_name(s_afe_vad_suppress_reason),
             (unsigned)duration_ms);
    s_afe_vad_suppressed = false;
    s_afe_vad_suppressed_tick = 0;
    s_afe_vad_suppress_reason = CONT_AFE_SUPPRESS_NONE;
    s_cont_start_hits = 0;
    cont_preroll_reset();
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
             "cont vad gated busy local=%d playback=%d mcp=%d rearm_left=%dms",
             s_audio_busy,
             s_assistant_playback_busy,
             mcp_client_is_assistant_busy(),
             now < s_cont_rearm_tick ? (int)((s_cont_rearm_tick - now) * portTICK_PERIOD_MS) : 0);
}

static void cont_vad_log_suppressed_gate(TickType_t now, int avg_abs, int peak, vad_state_t sr_vad_state)
{
    if ((uint32_t)((now - s_cont_busy_log_tick) * portTICK_PERIOD_MS) < CONT_VAD_LOG_INTERVAL_MS) {
        return;
    }
    s_cont_busy_log_tick = now;
    ESP_LOGI(TAG,
             "cont vad suppressed afe reason=%s avg=%d peak=%d sr=%d",
             cont_afe_suppress_reason_name(s_afe_vad_suppress_reason),
             avg_abs,
             peak,
             sr_vad_state == VAD_SPEECH ? 1 : 0);
}

static void cont_vad_log_playback_tail_gate(TickType_t now, int avg_abs, int peak, vad_state_t sr_vad_state)
{
    if ((uint32_t)((now - s_cont_tail_log_tick) * portTICK_PERIOD_MS) < CONT_VAD_LOG_INTERVAL_MS) {
        return;
    }
    s_cont_tail_log_tick = now;
    ESP_LOGI(TAG,
             "cont vad gated playback_tail left=%dms avg=%d peak=%d sr=%d",
             cont_playback_tail_gate_active(now) ? (int)((s_cont_playback_gate_until_tick - now) * portTICK_PERIOD_MS) : 0,
             avg_abs,
             peak,
             sr_vad_state == VAD_SPEECH ? 1 : 0);
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

static bool cont_playback_tail_gate_active(TickType_t now)
{
    if (s_cont_playback_tail_until_tick != 0 && now < s_cont_playback_tail_until_tick) {
        return true;
    }
    return s_cont_playback_gate_until_tick != 0 && now < s_cont_playback_gate_until_tick;
}

static cont_barge_raw_snapshot_t cont_barge_get_raw_snapshot(TickType_t now, bool *raw_valid)
{
    cont_barge_raw_snapshot_t snap = s_cont_barge_raw;
    bool valid = snap.frames > 0 &&
                 snap.channels > CONT_BARGE_MIC_CH &&
                 snap.tick != 0 &&
                 (uint32_t)((now - snap.tick) * portTICK_PERIOD_MS) <= CONT_BARGE_RAW_STALE_MS;
    if (raw_valid) {
        *raw_valid = valid;
    }
    return snap;
}

static void cont_barge_reset_candidate(void)
{
    s_cont_barge_candidate_active = false;
    s_cont_barge_candidate_playback = false;
    s_cont_barge_candidate_tail = false;
    s_cont_barge_candidate_start_tick = 0;
    s_cont_barge_candidate_log_tick = 0;
    s_cont_barge_accept_hits = 0;
    s_cont_barge_echo_hits = 0;
    s_cont_barge_start_local_override = false;
    s_cont_barge_max_afe_avg = 0;
    s_cont_barge_max_afe_peak = 0;
    s_cont_barge_max_ref_avg = 0;
    s_cont_barge_max_mic_avg = 0;
}

static void suppress_playback_for_barge(void)
{
    bool had_loop = s_loop_enabled;
    bool had_playback = s_assistant_playback_busy;
    bool had_remote_busy = mcp_client_is_assistant_busy();
    s_barge_playback_cancelled = had_playback;
    s_loop_enabled = false;
    mcp_client_cancel_playback();
    if (!had_playback) {
        s_barge_playback_cancelled = false;
    }
    if (had_loop) {
        app_ui_set_voice_state("BARGE LOOP OFF");
    }
    ESP_LOGI(TAG,
             "barge playback suppressed loop=%d playback=%d remote=%d",
             had_loop ? 1 : 0,
             had_playback ? 1 : 0,
             had_remote_busy ? 1 : 0);
}

static void cont_barge_begin_candidate(TickType_t now, bool playback, bool tail)
{
    if (s_cont_barge_candidate_active) {
        if (playback) {
            s_cont_barge_candidate_playback = true;
        }
        if (tail) {
            s_cont_barge_candidate_tail = true;
        }
        return;
    }

    bool raw_valid = false;
    cont_barge_raw_snapshot_t raw = cont_barge_get_raw_snapshot(now, &raw_valid);
    s_cont_barge_candidate_active = true;
    s_cont_barge_candidate_playback = playback;
    s_cont_barge_candidate_tail = tail;
    s_cont_barge_candidate_start_tick = now;
    s_cont_barge_candidate_log_tick = 0;
    s_cont_barge_accept_hits = 0;
    s_cont_barge_echo_hits = 0;
    s_cont_barge_start_local_override = playback &&
                                         raw_valid &&
                                         raw.mic_avg >= CONT_BARGE_OVERRIDE_MIC_AVG &&
                                         raw.mic_peak >= CONT_BARGE_OVERRIDE_MIC_PEAK &&
                                         raw.corr_permille <= CONT_BARGE_OVERRIDE_CORR_MAX;
    s_cont_barge_max_afe_avg = 0;
    s_cont_barge_max_afe_peak = 0;
    s_cont_barge_max_ref_avg = raw_valid ? raw.ref_avg : 0;
    s_cont_barge_max_mic_avg = raw_valid ? raw.mic_avg : 0;
    clear_afe_vad_pending();
    ESP_LOGI(TAG,
             "cont barge candidate start phase=%s raw_valid=%d ch0_ref avg=%d peak=%d ch3_mic avg=%d peak=%d corr=%d gain=%d/1000",
             playback ? "playback" : (tail ? "playback_tail" : "gate"),
             raw_valid,
             raw.ref_avg,
             raw.ref_peak,
             raw.mic_avg,
             raw.mic_peak,
             raw.corr_permille,
             s_cont_barge_echo_gain_permille);
    if (s_cont_barge_start_local_override) {
        ESP_LOGI(TAG,
                 "cont barge start local override mic_avg=%d mic_peak=%d corr=%d",
                 raw.mic_avg,
                 raw.mic_peak,
                 raw.corr_permille);
    }
}

static void cont_barge_log_decision(const char *decision,
                                    TickType_t now,
                                    int avg_abs,
                                    int peak,
                                    const cont_barge_raw_snapshot_t *raw,
                                    bool raw_valid,
                                    bool mic_excess,
                                    bool afe_strong)
{
    uint32_t age_ms = s_cont_barge_candidate_start_tick
                          ? (uint32_t)((now - s_cont_barge_candidate_start_tick) * portTICK_PERIOD_MS)
                          : 0;
    ESP_LOGI(TAG,
             "cont barge candidate %s phase=%s age=%ums afe avg=%d peak=%d max_avg=%d max_peak=%d raw_valid=%d ref avg=%d peak=%d mic avg=%d peak=%d corr=%d gain=%d/1000 mic_excess=%d afe_strong=%d hits=%d echo_hits=%d",
             decision,
             s_cont_barge_candidate_playback ? "playback" : (s_cont_barge_candidate_tail ? "playback_tail" : "gate"),
             (unsigned)age_ms,
             avg_abs,
             peak,
             s_cont_barge_max_afe_avg,
             s_cont_barge_max_afe_peak,
             raw_valid,
             raw ? raw->ref_avg : 0,
             raw ? raw->ref_peak : 0,
             raw ? raw->mic_avg : 0,
             raw ? raw->mic_peak : 0,
             raw ? raw->corr_permille : 0,
             s_cont_barge_echo_gain_permille,
             mic_excess,
             afe_strong,
             s_cont_barge_accept_hits,
             s_cont_barge_echo_hits);
}

static void cont_barge_get_thresholds(int *confirm_avg,
                                      int *confirm_peak,
                                      int *strong_avg,
                                      int *strong_peak)
{
    bool rnnm_path = cont_using_rnnm_path();
    int confirm_base = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_AVG : CONT_AFE_VAD_CONFIRM_AVG;
    int confirm_margin = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_MARGIN : CONT_AFE_VAD_CONFIRM_MARGIN;
    int confirm_peak_value = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_PEAK : CONT_AFE_VAD_CONFIRM_PEAK;
    int strong_base = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_AVG : CONT_AFE_VAD_CONFIRM_STRONG_AVG;
    int strong_margin = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_MARGIN : CONT_AFE_VAD_CONFIRM_STRONG_MARGIN;
    int strong_peak_value = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_PEAK : CONT_AFE_VAD_CONFIRM_STRONG_PEAK;
    if (confirm_avg) {
        *confirm_avg = max_int(confirm_base, s_cont_noise_floor + confirm_margin);
    }
    if (confirm_peak) {
        *confirm_peak = confirm_peak_value;
    }
    if (strong_avg) {
        *strong_avg = max_int(strong_base, s_cont_noise_floor + strong_margin);
    }
    if (strong_peak) {
        *strong_peak = strong_peak_value;
    }
}

static bool cont_barge_compute_rule(TickType_t now,
                                    int avg_abs,
                                    int peak,
                                    vad_state_t sr_vad_state,
                                    cont_barge_raw_snapshot_t *raw_out,
                                    bool *raw_valid_out,
                                    bool *mic_excess_out,
                                    bool *afe_strong_out,
                                    bool *echo_like_out,
                                    bool *barge_hit_out)
{
    bool raw_valid = false;
    cont_barge_raw_snapshot_t raw = cont_barge_get_raw_snapshot(now, &raw_valid);
    int confirm_avg = 0;
    int confirm_peak = 0;
    int strong_avg = 0;
    int strong_peak = 0;
    cont_barge_get_thresholds(&confirm_avg, &confirm_peak, &strong_avg, &strong_peak);

    int predicted_mic_avg = raw.ref_avg * s_cont_barge_echo_gain_permille / 1000;
    int predicted_mic_peak = raw.ref_peak * s_cont_barge_echo_gain_permille / 1000;
    int effective_avg = max_int(avg_abs, s_cont_barge_max_afe_avg);
    int effective_peak = max_int(peak, s_cont_barge_max_afe_peak);
    bool ref_active = raw_valid &&
                      (raw.ref_avg >= CONT_BARGE_REF_ACTIVE_AVG ||
                       raw.ref_peak >= CONT_BARGE_REF_ACTIVE_PEAK);
    bool corr_high = raw_valid && raw.corr_permille >= CONT_BARGE_CORR_HIGH;
    bool corr_low = !raw_valid || raw.corr_permille <= CONT_BARGE_CORR_LOW;
    bool mic_excess = raw_valid &&
                      raw.mic_avg >= predicted_mic_avg + CONT_BARGE_MIC_EXCESS_AVG &&
                      raw.mic_peak >= predicted_mic_peak + CONT_BARGE_MIC_EXCESS_PEAK;
    bool local_mic_energy = raw_valid &&
                            raw.mic_avg >= CONT_BARGE_LOCAL_MIC_AVG &&
                            raw.mic_peak >= CONT_BARGE_LOCAL_MIC_PEAK;
    bool afe_voice = ((sr_vad_state == VAD_SPEECH) &&
                      effective_avg >= confirm_avg &&
                      effective_peak >= confirm_peak);
    bool afe_strong = effective_avg >= strong_avg && effective_peak >= strong_peak;
    bool echo_like = ref_active &&
                     corr_high &&
                     !afe_strong;
    bool very_strong_without_raw = !raw_valid &&
                                   effective_avg >= strong_avg + CONT_BARGE_STRONG_EXTRA_AVG &&
                                   effective_peak >= strong_peak + CONT_BARGE_STRONG_EXTRA_PEAK;
    bool local_dominant = raw_valid &&
                          ((ref_active && mic_excess && (!corr_high || afe_strong)) ||
                           (!ref_active && (corr_low || local_mic_energy)));
    bool barge_hit = !echo_like &&
                     (afe_voice || afe_strong) &&
                     (local_dominant ||
                      (afe_strong &&
                       effective_avg >= strong_avg + CONT_BARGE_STRONG_EXTRA_AVG &&
                       effective_peak >= strong_peak + CONT_BARGE_STRONG_EXTRA_PEAK) ||
                      very_strong_without_raw);

    if (raw_out) {
        *raw_out = raw;
    }
    if (raw_valid_out) {
        *raw_valid_out = raw_valid;
    }
    if (mic_excess_out) {
        *mic_excess_out = mic_excess;
    }
    if (afe_strong_out) {
        *afe_strong_out = afe_strong;
    }
    if (echo_like_out) {
        *echo_like_out = echo_like;
    }
    if (barge_hit_out) {
        *barge_hit_out = barge_hit;
    }
    return barge_hit;
}

static bool cont_barge_tail_probe_hit(TickType_t now, int avg_abs, int peak, vad_state_t sr_vad_state)
{
    cont_barge_raw_snapshot_t raw = {0};
    bool raw_valid = false;
    bool mic_excess = false;
    bool afe_strong = false;
    bool echo_like = false;
    bool barge_hit = false;
    cont_barge_compute_rule(now,
                            avg_abs,
                            peak,
                            sr_vad_state,
                            &raw,
                            &raw_valid,
                            &mic_excess,
                            &afe_strong,
                            &echo_like,
                            &barge_hit);
    return barge_hit && !echo_like;
}

static cont_barge_decision_t cont_barge_process_candidate(TickType_t now,
                                                          int avg_abs,
                                                          int peak,
                                                          vad_state_t sr_vad_state)
{
    if (!s_cont_barge_candidate_active) {
        return CONT_BARGE_DECISION_REJECT;
    }

    if (avg_abs > s_cont_barge_max_afe_avg) {
        s_cont_barge_max_afe_avg = avg_abs;
    }
    if (peak > s_cont_barge_max_afe_peak) {
        s_cont_barge_max_afe_peak = peak;
    }

    cont_barge_raw_snapshot_t raw = {0};
    bool raw_valid = false;
    bool mic_excess = false;
    bool afe_strong = false;
    bool echo_like = false;
    bool barge_hit = false;
    cont_barge_compute_rule(now,
                            avg_abs,
                            peak,
                            sr_vad_state,
                            &raw,
                            &raw_valid,
                            &mic_excess,
                            &afe_strong,
                            &echo_like,
                            &barge_hit);
    if (raw_valid) {
        if (raw.ref_avg > s_cont_barge_max_ref_avg) {
            s_cont_barge_max_ref_avg = raw.ref_avg;
        }
        if (raw.mic_avg > s_cont_barge_max_mic_avg) {
            s_cont_barge_max_mic_avg = raw.mic_avg;
        }
    }

    if (barge_hit) {
        ++s_cont_barge_accept_hits;
    } else if (s_cont_barge_accept_hits > 0) {
        --s_cont_barge_accept_hits;
    }
    if (echo_like) {
        ++s_cont_barge_echo_hits;
    }

    uint32_t age_ms = (uint32_t)((now - s_cont_barge_candidate_start_tick) * portTICK_PERIOD_MS);
    if (age_ms < CONT_BARGE_CANDIDATE_MIN_MS) {
        return CONT_BARGE_DECISION_PENDING;
    }
    if (s_cont_barge_start_local_override) {
        cont_barge_log_decision("accept_local_override", now, avg_abs, peak, &raw, raw_valid, mic_excess, afe_strong);
        return CONT_BARGE_DECISION_ACCEPT;
    }
    if (s_cont_barge_accept_hits >= CONT_BARGE_ACCEPT_HITS) {
        cont_barge_log_decision("accept", now, avg_abs, peak, &raw, raw_valid, mic_excess, afe_strong);
        return CONT_BARGE_DECISION_ACCEPT;
    }
    if (s_cont_barge_echo_hits >= CONT_BARGE_ECHO_REJECT_HITS || age_ms >= CONT_BARGE_CANDIDATE_MAX_MS) {
        cont_barge_log_decision(s_cont_barge_echo_hits >= CONT_BARGE_ECHO_REJECT_HITS ? "reject_echo" : "reject_timeout",
                                now,
                                avg_abs,
                                peak,
                                &raw,
                                raw_valid,
                                mic_excess,
                                afe_strong);
        return CONT_BARGE_DECISION_REJECT;
    }
    return CONT_BARGE_DECISION_PENDING;
}

static bool cont_barge_accept_candidate(TickType_t now,
                                        int avg_abs,
                                        int peak,
                                        vad_state_t sr_vad_state)
{
    make_capture_session_id();
    s_capture_chunk_len = 0;
    esp_err_t err = mcp_client_audio_stream_begin(s_capture_session_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "barge stream begin failed: %s", esp_err_to_name(err));
        s_capture_session_id[0] = '\0';
        cont_barge_reset_candidate();
        app_ui_set_mcp_status("MCP SEND FAIL");
        return false;
    }

    s_continuous_speaking = true;
    s_cont_speech_start_tick = now;
    s_cont_last_voice_tick = now;
    s_cont_last_audio_tick = now;
    s_cont_silence_hits = 0;
    s_cont_utterance_peak_avg = avg_abs;
    cont_preroll_flush_to_capture();
    suppress_playback_for_barge();
    clear_afe_vad_pending();
    cont_barge_reset_candidate();
    app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
    app_ui_set_mic_state("BARGE REC");
    ESP_LOGI(TAG,
             "barge utterance start session=%s avg=%d peak=%d sr=%d assistant_busy=%d tail_gate_left=%dms",
             s_capture_session_id,
             avg_abs,
             peak,
             sr_vad_state == VAD_SPEECH ? 1 : 0,
             s_assistant_playback_busy,
             cont_playback_tail_gate_active(now) ? (int)((s_cont_playback_gate_until_tick - now) * portTICK_PERIOD_MS) : 0);
    return true;
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
        bool playback_busy = s_assistant_playback_busy;
        bool playback_tail_gate = cont_playback_tail_gate_active(now);
        bool started_now = false;
        if (afe_vad_path &&
            !s_continuous_speaking &&
            s_afe_vad_suppressed &&
            s_afe_vad_suppress_reason == CONT_AFE_SUPPRESS_BARGE_REJECT &&
            (playback_busy || playback_tail_gate) &&
            now >= s_afe_vad_reject_until_tick) {
            cont_afe_clear_suppressed();
            cont_barge_begin_candidate(now, playback_busy, playback_tail_gate);
        }
        if (afe_vad_path && !s_continuous_speaking && s_afe_vad_suppressed) {
            cont_vad_log_suppressed_gate(now, avg_abs, peak, sr_vad_state);
            cont_preroll_reset();
            return;
        }
        if (afe_vad_path &&
            !s_continuous_speaking &&
            (playback_busy || playback_tail_gate || s_cont_barge_candidate_active)) {
            cont_preroll_store(data, len);
            if (!s_cont_barge_candidate_active &&
                playback_tail_gate &&
                cont_barge_tail_probe_hit(now, avg_abs, peak, sr_vad_state)) {
                cont_barge_begin_candidate(now, false, true);
            }
            if (s_cont_barge_candidate_active) {
                cont_barge_decision_t decision = cont_barge_process_candidate(now, avg_abs, peak, sr_vad_state);
                if (decision == CONT_BARGE_DECISION_ACCEPT) {
                    if (!cont_barge_accept_candidate(now, avg_abs, peak, sr_vad_state)) {
                        return;
                    }
                    started_now = true;
                } else {
                    if (decision == CONT_BARGE_DECISION_REJECT) {
                        extend_afe_reject_until(now + pdMS_TO_TICKS(CONT_PLAYBACK_TAIL_REJECT_MS));
                        cont_barge_reset_candidate();
                        cont_afe_suppress_segment(now, CONT_AFE_SUPPRESS_BARGE_REJECT);
                        cont_preroll_reset();
                    }
                    if (playback_busy) {
                        cont_vad_log_busy_gate(now);
                    } else {
                        cont_vad_log_playback_tail_gate(now, avg_abs, peak, sr_vad_state);
                    }
                    return;
                }
            } else {
                if (playback_busy) {
                    cont_vad_log_busy_gate(now);
                } else {
                    cont_vad_log_playback_tail_gate(now, avg_abs, peak, sr_vad_state);
                }
                cont_preroll_reset();
                return;
            }
        }
        if (!s_continuous_speaking && assistant_busy) {
            cont_vad_log_busy_gate(now);
            return;
        }
        if (!s_continuous_speaking && playback_tail_gate) {
            cont_vad_log_playback_tail_gate(now, avg_abs, peak, sr_vad_state);
            cont_preroll_reset();
            return;
        }
        if (!s_continuous_speaking && now < s_cont_rearm_tick) {
            cont_vad_log_busy_gate(now);
            return;
        }
        if (afe_vad_path && !s_continuous_speaking) {
            cont_preroll_store(data, len);
            cont_vad_update_noise_floor(avg_abs);
            cont_vad_log_sample_ex(now, avg_abs, peak, 0, sr_vad_state);
            if (s_afe_vad_pending) {
                uint32_t pending_ms = (uint32_t)((now - s_afe_vad_pending_tick) * portTICK_PERIOD_MS);
                if (pending_ms > CONT_AFE_VAD_CONFIRM_MS) {
                    ESP_LOGI(TAG, "afe vad confirm timeout avg=%d peak=%d", avg_abs, peak);
                    extend_afe_reject_until(now + pdMS_TO_TICKS(CONT_AFE_REJECT_REARM_MS));
                    cont_afe_suppress_segment(now, CONT_AFE_SUPPRESS_CONFIRM_TIMEOUT);
                    return;
                }
                bool rnnm_path = cont_using_rnnm_path();
                int confirm_base = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_AVG : CONT_AFE_VAD_CONFIRM_AVG;
                int confirm_margin = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_MARGIN : CONT_AFE_VAD_CONFIRM_MARGIN;
                int confirm_peak = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_PEAK : CONT_AFE_VAD_CONFIRM_PEAK;
                int confirm_hits = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_HITS : CONT_AFE_VAD_CONFIRM_HITS;
                int strong_base = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_AVG : CONT_AFE_VAD_CONFIRM_STRONG_AVG;
                int strong_margin = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_MARGIN : CONT_AFE_VAD_CONFIRM_STRONG_MARGIN;
                int strong_peak = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_PEAK : CONT_AFE_VAD_CONFIRM_STRONG_PEAK;
                int confirm_avg = max_int(confirm_base, s_cont_noise_floor + confirm_margin);
                int strong_avg = max_int(strong_base, s_cont_noise_floor + strong_margin);
                bool sr_energy_hit = (sr_vad_state == VAD_SPEECH) &&
                                     avg_abs >= confirm_avg &&
                                     peak >= confirm_peak;
                bool strong_energy_hit = avg_abs >= strong_avg &&
                                         peak >= strong_peak;
                if (sr_energy_hit || strong_energy_hit) {
                    ++s_afe_vad_confirm_hits;
                } else if (s_afe_vad_confirm_hits > 0) {
                    --s_afe_vad_confirm_hits;
                }
                if (s_afe_vad_confirm_hits < confirm_hits) {
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
                         confirm_hits,
                         sr_vad_state == VAD_SPEECH ? 1 : 0);
            }
        }
        if (afe_vad_path && !s_continuous_speaking && now < s_afe_vad_reject_until_tick) {
            return;
        }
        if (afe_vad_path && !s_continuous_speaking) {
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
                bool rnnm_path = cont_using_rnnm_path();
                int local_start_avg = rnnm_path ? CONT_RNNM_AFE_LOCAL_START_AVG : CONT_AFE_LOCAL_START_AVG;
                int local_start_margin = rnnm_path ? CONT_RNNM_AFE_LOCAL_START_MARGIN : CONT_AFE_LOCAL_START_MARGIN;
                int strong_base = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_AVG : CONT_AFE_VAD_CONFIRM_STRONG_AVG;
                int strong_margin = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_MARGIN : CONT_AFE_VAD_CONFIRM_STRONG_MARGIN;
                int strong_peak = rnnm_path ? CONT_RNNM_AFE_VAD_CONFIRM_STRONG_PEAK : CONT_AFE_VAD_CONFIRM_STRONG_PEAK;
                start_threshold = max_int(local_start_avg, s_cont_noise_floor + local_start_margin);
                start_peak = rnnm_path ? CONT_RNNM_AFE_LOCAL_START_PEAK : CONT_AFE_LOCAL_START_PEAK;
                start_hits = rnnm_path ? CONT_RNNM_AFE_LOCAL_START_HITS : CONT_AFE_LOCAL_START_HITS;
                int strong_avg = max_int(strong_base, s_cont_noise_floor + strong_margin);
                sr_hit = (sr_vad_state == VAD_SPEECH) &&
                         avg_abs >= start_threshold &&
                         peak >= start_peak;
                bool strong_energy_hit = avg_abs >= strong_avg &&
                                         peak >= strong_peak;
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
            int confirm_peak = cont_using_rnnm_path() ? CONT_RNNM_AFE_VAD_CONFIRM_PEAK : CONT_AFE_VAD_CONFIRM_PEAK;
            bool sr_energy_active = (sr_vad_state == VAD_SPEECH) &&
                                    avg_abs >= stop_threshold &&
                                    peak >= confirm_peak;
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
    if (s_audio_busy || s_assistant_playback_busy) {
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
    s_assistant_playback_busy = false;
    s_cont_playback_tail_until_tick = 0;
    s_cont_playback_gate_until_tick = 0;
    s_cont_rearm_tick = xTaskGetTickCount() + pdMS_TO_TICKS(CONT_STARTUP_REARM_MS);
    memset(&s_cont_barge_raw, 0, sizeof(s_cont_barge_raw));
    memset(s_cont_barge_ref_env, 0, sizeof(s_cont_barge_ref_env));
    memset(s_cont_barge_mic_env, 0, sizeof(s_cont_barge_mic_env));
    s_cont_barge_env_pos = 0;
    s_cont_barge_env_count = 0;
    s_cont_barge_echo_gain_permille = CONT_BARGE_ECHO_GAIN_INIT;
    cont_vad_reset_runtime();
    cont_vad_reset_noise_floor();
    cont_preroll_reset();
    s_capture_session_id[0] = '\0';
    s_capture_chunk_len = 0;

    if (s_afe_ready) {
        afe_capture_set_raw_audio_callback(on_raw_tdm_audio, NULL);
    }
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
        if (s_afe_ready) {
            afe_capture_set_raw_audio_callback(NULL, NULL);
        }
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
    ESP_LOGI(TAG,
             "continuous chat enabled path=%s startup_rearm=%dms",
             s_afe_ready ? "afe_aec" : "mono_pcm",
             CONT_STARTUP_REARM_MS);
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
        afe_capture_set_raw_audio_callback(NULL, NULL);
    } else if (s_mic_diag_ready && mic_diag_is_capturing() && s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        mic_diag_capture_stop();
    }
    s_continuous_chat = false;
    s_continuous_speaking = false;
    s_capture_mode = CAPTURE_MODE_NONE;
    s_audio_busy = false;
    s_assistant_playback_busy = false;
    s_cont_playback_tail_until_tick = 0;
    s_cont_playback_gate_until_tick = 0;
    memset(&s_cont_barge_raw, 0, sizeof(s_cont_barge_raw));
    memset(s_cont_barge_ref_env, 0, sizeof(s_cont_barge_ref_env));
    memset(s_cont_barge_mic_env, 0, sizeof(s_cont_barge_mic_env));
    s_cont_barge_env_pos = 0;
    s_cont_barge_env_count = 0;
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

static void diag_stats_update_sample(raw_tdm_channel_stats_t *stats, int16_t sample)
{
    if (!stats) {
        return;
    }
    int level = abs16_sample(sample);
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
}

static void diag_stats_update_pcm(raw_tdm_channel_stats_t *stats, const uint8_t *data, int len)
{
    if (!stats || !data || len <= 0) {
        return;
    }
    int sample_count = (len & ~(int)1) / (int)sizeof(int16_t);
    const int16_t *samples = (const int16_t *)data;
    for (int i = 0; i < sample_count; ++i) {
        diag_stats_update_sample(stats, samples[i]);
    }
}

static uint32_t diag_stats_avg(const raw_tdm_channel_stats_t *stats)
{
    return stats && stats->samples ? (uint32_t)(stats->sum_abs / stats->samples) : 0;
}

static uint32_t diag_stats_rms(const raw_tdm_channel_stats_t *stats)
{
    return stats && stats->samples ? isqrt_u64_local(stats->sum_sq / stats->samples) : 0;
}

static uint32_t diag_stats_zcr_pm(const raw_tdm_channel_stats_t *stats)
{
    return stats && stats->samples > 1 ? (stats->zero_crossings * 1000) / (stats->samples - 1) : 0;
}

static void ref_env_reset(ref_env_track_t *env)
{
    if (!env) {
        return;
    }
    env->count = 0;
    env->current_frames = 0;
    env->current_sum_abs = 0;
}

static bool ref_env_ensure(ref_env_track_t *env)
{
    if (!env) {
        return false;
    }
    if (env->blocks) {
        return true;
    }
    env->blocks = heap_caps_calloc(BARGE_REF_ENV_MAX_BLOCKS, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!env->blocks) {
        env->blocks = heap_caps_calloc(BARGE_REF_ENV_MAX_BLOCKS, sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!env->blocks) {
        ESP_LOGW(TAG, "barge ref corr env alloc failed");
        return false;
    }
    ref_env_reset(env);
    return true;
}

static bool barge_ref_corr_ensure_buffers(void)
{
    if (!ref_env_ensure(&s_barge_ref_env)) {
        return false;
    }
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        if (!ref_env_ensure(&s_barge_raw_env[ch])) {
            return false;
        }
    }
    return true;
}

static void ref_env_push_level(ref_env_track_t *env, int level)
{
    if (!env || !env->blocks || env->count >= BARGE_REF_ENV_MAX_BLOCKS) {
        return;
    }
    env->current_sum_abs += (uint32_t)level;
    ++env->current_frames;
    if (env->current_frames >= BARGE_REF_ENV_BLOCK_FRAMES) {
        uint32_t avg = (uint32_t)(env->current_sum_abs / env->current_frames);
        env->blocks[env->count++] = (uint16_t)(avg > 65535U ? 65535U : avg);
        env->current_sum_abs = 0;
        env->current_frames = 0;
    }
}

static void ref_env_flush_partial(ref_env_track_t *env)
{
    if (!env || !env->blocks || env->current_frames == 0 || env->count >= BARGE_REF_ENV_MAX_BLOCKS) {
        return;
    }
    uint32_t avg = (uint32_t)(env->current_sum_abs / env->current_frames);
    env->blocks[env->count++] = (uint16_t)(avg > 65535U ? 65535U : avg);
    env->current_sum_abs = 0;
    env->current_frames = 0;
}

static void ref_env_push_mono(ref_env_track_t *env, const int16_t *samples, int frames)
{
    if (!env || !samples || frames <= 0) {
        return;
    }
    for (int i = 0; i < frames; ++i) {
        ref_env_push_level(env, abs16_sample(samples[i]));
    }
}

static void barge_ref_corr_reset(void)
{
    memset(&s_barge_ref_corr, 0, sizeof(s_barge_ref_corr));
    s_barge_ref_corr.channel = -1;
    if (!barge_ref_corr_ensure_buffers()) {
        return;
    }
    ref_env_reset(&s_barge_ref_env);
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        ref_env_reset(&s_barge_raw_env[ch]);
    }
}

static void on_barge_reference_audio(const int16_t *samples, int frames, void *ctx)
{
    (void)ctx;
    if (!s_barge_diag_active || !s_barge_diag_playing) {
        return;
    }
    ref_env_push_mono(&s_barge_ref_env, samples, frames);
}

static int compute_env_corr_permille(const uint16_t *ref,
                                     int ref_count,
                                     const uint16_t *raw,
                                     int raw_count,
                                     int lag_blocks,
                                     int *gain_permille)
{
    if (!ref || !raw || ref_count <= 0 || raw_count <= 0 || lag_blocks < 0 || lag_blocks >= raw_count) {
        return 0;
    }
    int n = ref_count;
    if (raw_count - lag_blocks < n) {
        n = raw_count - lag_blocks;
    }
    if (n < BARGE_REF_ENV_MIN_OVERLAP_BLOCKS) {
        return 0;
    }

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_x2 = 0.0;
    double sum_y2 = 0.0;
    double sum_xy = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = (double)ref[i];
        double y = (double)raw[i + lag_blocks];
        sum_x += x;
        sum_y += y;
        sum_x2 += x * x;
        sum_y2 += y * y;
        sum_xy += x * y;
    }
    double var_x = (double)n * sum_x2 - (sum_x * sum_x);
    double var_y = (double)n * sum_y2 - (sum_y * sum_y);
    double score = 0.0;
    if (var_x > 1.0 && var_y > 1.0) {
        score = (((double)n * sum_xy) - (sum_x * sum_y)) / sqrt(var_x * var_y);
    }
    if (score < 0.0) {
        score = -score;
    }
    if (score > 1.0) {
        score = 1.0;
    }
    if (gain_permille) {
        *gain_permille = sum_x > 1.0 ? (int)((sum_y * 1000.0) / sum_x) : 0;
    }
    return (int)(score * 1000.0 + 0.5);
}

static ref_corr_result_t barge_ref_corr_analyze(void)
{
    ref_env_flush_partial(&s_barge_ref_env);
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        ref_env_flush_partial(&s_barge_raw_env[ch]);
    }

    ref_corr_result_t best = {
        .channel = -1,
        .score_permille = 0,
        .lag_blocks = 0,
        .lag_ms = 0,
        .gain_permille = 0,
        .ref_blocks = s_barge_ref_env.count,
        .raw_blocks = 0,
    };
    int ref_count = (int)s_barge_ref_env.count;
    if (ref_count < BARGE_REF_ENV_MIN_OVERLAP_BLOCKS) {
        return best;
    }

    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        int raw_count = (int)s_barge_raw_env[ch].count;
        if (raw_count < BARGE_REF_ENV_MIN_OVERLAP_BLOCKS) {
            continue;
        }
        int max_lag = raw_count - BARGE_REF_ENV_MIN_OVERLAP_BLOCKS;
        if (max_lag > BARGE_REF_ENV_MAX_LAG_BLOCKS) {
            max_lag = BARGE_REF_ENV_MAX_LAG_BLOCKS;
        }
        for (int lag = 0; lag <= max_lag; ++lag) {
            int gain_permille = 0;
            int score = compute_env_corr_permille(s_barge_ref_env.blocks,
                                                  ref_count,
                                                  s_barge_raw_env[ch].blocks,
                                                  raw_count,
                                                  lag,
                                                  &gain_permille);
            if (score > best.score_permille) {
                best.channel = ch;
                best.score_permille = score;
                best.lag_blocks = lag;
                best.lag_ms = (lag * BARGE_REF_ENV_BLOCK_FRAMES * 1000) / RAW_TDM_DIAG_RATE;
                best.gain_permille = gain_permille;
                best.raw_blocks = (uint32_t)raw_count;
            }
        }
    }
    return best;
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

static void queue_barge_diag_finish(void)
{
    if (!s_barge_diag_active || s_barge_diag_finish_queued) {
        return;
    }
    s_barge_diag_finish_queued = true;
    send_cmd_nonblocking(VOICE_CMD_BARGE_DIAG_DONE, 0);
}

static void on_cont_barge_raw_audio(const int16_t *interleaved, int frames, int channels)
{
    if (!s_continuous_chat ||
        s_capture_mode != CAPTURE_MODE_CONTINUOUS ||
        !interleaved ||
        frames <= 0 ||
        channels <= CONT_BARGE_MIC_CH) {
        return;
    }

    int ref_peak = 0;
    int mic_peak = 0;
    int64_t ref_sum = 0;
    int64_t mic_sum = 0;
    for (int frame = 0; frame < frames; ++frame) {
        const int16_t *row = interleaved + frame * channels;
        int ref_level = abs16_sample(row[CONT_BARGE_REF_CH]);
        int mic_level = abs16_sample(row[CONT_BARGE_MIC_CH]);
        if (ref_level > ref_peak) {
            ref_peak = ref_level;
        }
        if (mic_level > mic_peak) {
            mic_peak = mic_level;
        }
        ref_sum += ref_level;
        mic_sum += mic_level;
    }

    int ref_avg = frames > 0 ? (int)(ref_sum / frames) : 0;
    int mic_avg = frames > 0 ? (int)(mic_sum / frames) : 0;
    s_cont_barge_ref_env[s_cont_barge_env_pos] = (uint16_t)(ref_avg > 65535 ? 65535 : ref_avg);
    s_cont_barge_mic_env[s_cont_barge_env_pos] = (uint16_t)(mic_avg > 65535 ? 65535 : mic_avg);
    s_cont_barge_env_pos = (s_cont_barge_env_pos + 1) % CONT_BARGE_RAW_CORR_WINDOW;
    if (s_cont_barge_env_count < CONT_BARGE_RAW_CORR_WINDOW) {
        ++s_cont_barge_env_count;
    }

    uint16_t ref_ordered[CONT_BARGE_RAW_CORR_WINDOW];
    uint16_t mic_ordered[CONT_BARGE_RAW_CORR_WINDOW];
    int count = s_cont_barge_env_count;
    int start = (s_cont_barge_env_pos + CONT_BARGE_RAW_CORR_WINDOW - count) % CONT_BARGE_RAW_CORR_WINDOW;
    for (int i = 0; i < count; ++i) {
        int idx = (start + i) % CONT_BARGE_RAW_CORR_WINDOW;
        ref_ordered[i] = s_cont_barge_ref_env[idx];
        mic_ordered[i] = s_cont_barge_mic_env[idx];
    }
    int corr_permille = compute_env_pair_corr_permille(ref_ordered, mic_ordered, count);

    s_cont_barge_raw.tick = xTaskGetTickCount();
    s_cont_barge_raw.frames = frames;
    s_cont_barge_raw.channels = channels;
    s_cont_barge_raw.ref_avg = ref_avg;
    s_cont_barge_raw.ref_peak = ref_peak;
    s_cont_barge_raw.mic_avg = mic_avg;
    s_cont_barge_raw.mic_peak = mic_peak;
    s_cont_barge_raw.corr_permille = corr_permille;

    if (!s_continuous_speaking &&
        s_assistant_playback_busy &&
        ref_avg >= CONT_BARGE_REF_ACTIVE_AVG &&
        mic_avg > 0 &&
        corr_permille >= CONT_BARGE_CORR_HIGH) {
        int gain = ref_avg > 0 ? (mic_avg * 1000) / ref_avg : s_cont_barge_echo_gain_permille;
        gain = clamp_int(gain, CONT_BARGE_ECHO_GAIN_MIN, CONT_BARGE_ECHO_GAIN_MAX);
        s_cont_barge_echo_gain_permille = (s_cont_barge_echo_gain_permille * 7 + gain) / 8;
    }
}

static void on_barge_diag_raw_audio(const int16_t *interleaved, int frames, int channels)
{
    if (!s_barge_diag_active || !interleaved || frames <= 0 || channels <= 0) {
        return;
    }
    int channel_count = channels < RAW_TDM_DIAG_CHANNELS ? channels : RAW_TDM_DIAG_CHANNELS;
    s_barge_diag_channels_seen = channels;
    raw_tdm_channel_stats_t *stats = s_barge_diag_playing ? s_barge_diag_raw_play_stats : s_barge_diag_raw_post_stats;
    for (int frame = 0; frame < frames; ++frame) {
        const int16_t *row = interleaved + frame * channels;
        for (int ch = 0; ch < channel_count; ++ch) {
            diag_stats_update_sample(&stats[ch], row[ch]);
            ref_env_push_level(&s_barge_raw_env[ch], abs16_sample(row[ch]));
        }
    }

    TickType_t now = xTaskGetTickCount();
    if ((uint32_t)((now - s_barge_diag_log_tick) * portTICK_PERIOD_MS) >= BARGE_DIAG_LOG_INTERVAL_MS) {
        s_barge_diag_log_tick = now;
        raw_tdm_channel_stats_t *afe_stats = s_barge_diag_playing ? &s_barge_diag_afe_play_stats : &s_barge_diag_afe_post_stats;
        bool in_tail = !s_barge_diag_playing &&
                       s_barge_diag_play_tail_end_tick &&
                       now < s_barge_diag_play_tail_end_tick;
        ESP_LOGI(TAG,
                 "barge diag phase=%s raw_ch=%d afe_peak=%d afe_avg=%u vad_start=%u play_vad=%u tail_vad=%u wake=%u",
                 s_barge_diag_playing ? "playback" : (in_tail ? "playback_tail" : "post"),
                 channels,
                 afe_stats->peak,
                 (unsigned)diag_stats_avg(afe_stats),
                 (unsigned)s_barge_diag_vad_start_count,
                 (unsigned)s_barge_diag_vad_start_play_count,
                 (unsigned)s_barge_diag_vad_start_tail_count,
                 (unsigned)s_barge_diag_wake_count);
    }
    if (now >= s_barge_diag_end_tick) {
        queue_barge_diag_finish();
    }
}

static void on_barge_diag_afe_audio(const uint8_t *data, int len)
{
    if (!s_barge_diag_active || !data || len <= 0) {
        return;
    }
    raw_tdm_channel_stats_t *stats = s_barge_diag_playing ? &s_barge_diag_afe_play_stats : &s_barge_diag_afe_post_stats;
    diag_stats_update_pcm(stats, data, len);
}

static void on_raw_tdm_audio(const int16_t *interleaved, int frames, int channels, void *ctx)
{
    (void)ctx;
    on_cont_barge_raw_audio(interleaved, frames, channels);
    on_barge_diag_raw_audio(interleaved, frames, channels);
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
    on_barge_diag_afe_audio(data, len);
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

static void barge_diag_reset_state(void)
{
    s_barge_diag_active = false;
    s_barge_diag_finish_queued = false;
    s_barge_diag_playing = false;
    s_barge_diag_start_tick = 0;
    s_barge_diag_end_tick = 0;
    s_barge_diag_play_start_tick = 0;
    s_barge_diag_play_end_tick = 0;
    s_barge_diag_log_tick = 0;
    s_barge_diag_target_ms = 0;
    s_barge_diag_vad_start_count = 0;
    s_barge_diag_vad_end_count = 0;
    s_barge_diag_vad_start_play_count = 0;
    s_barge_diag_vad_start_tail_count = 0;
    s_barge_diag_vad_start_post_count = 0;
    s_barge_diag_wake_count = 0;
    s_barge_diag_channels_seen = 0;
    s_barge_diag_prompt_id = BARGE_DIAG_PROMPT_XIAOLE;
    s_barge_diag_play_tail_end_tick = 0;
    memset(s_barge_diag_raw_play_stats, 0, sizeof(s_barge_diag_raw_play_stats));
    memset(s_barge_diag_raw_post_stats, 0, sizeof(s_barge_diag_raw_post_stats));
    memset(&s_barge_diag_afe_play_stats, 0, sizeof(s_barge_diag_afe_play_stats));
    memset(&s_barge_diag_afe_post_stats, 0, sizeof(s_barge_diag_afe_post_stats));
    barge_ref_corr_reset();
}

static void log_barge_diag_stats_row(const char *phase, const char *name, const raw_tdm_channel_stats_t *stats)
{
    ESP_LOGI(TAG,
             "barge diag %s %s samples=%u peak=%d avg=%u rms=%u zcr=%u/1000",
             phase,
             name,
             stats ? (unsigned)stats->samples : 0,
             stats ? stats->peak : 0,
             (unsigned)diag_stats_avg(stats),
             (unsigned)diag_stats_rms(stats),
             (unsigned)diag_stats_zcr_pm(stats));
}

static esp_err_t play_barge_diag_prompt(int prompt_id)
{
    switch (normalize_barge_diag_prompt_id(prompt_id)) {
        case BARGE_DIAG_PROMPT_AFTER:
            return audio_player_play_barge_after_prompt();
        case BARGE_DIAG_PROMPT_INTERRUPT: {
            esp_err_t ret = ESP_OK;
            for (int i = 0; i < BARGE_DIAG_INTERRUPT_REPEATS; ++i) {
                ret = audio_player_play_barge_interrupt_prompt();
                if (ret != ESP_OK) {
                    break;
                }
            }
            return ret;
        }
        case BARGE_DIAG_PROMPT_XIAOLE:
        default:
            return audio_player_play_xiaole();
    }
}

static void start_barge_diag(int encoded_value)
{
    if (s_barge_diag_active) {
        ESP_LOGW(TAG, "barge diag already active");
        return;
    }
    if (s_raw_tdm_active) {
        ESP_LOGW(TAG, "barge diag skipped because raw TDM diag is active");
        return;
    }

    int duration_ms = barge_diag_value_duration(encoded_value);
    int fmt_id = barge_diag_value_fmt_id(encoded_value);
    int prompt_id = barge_diag_value_prompt_id(encoded_value);
    const char *requested_fmt = barge_diag_fmt_from_id(fmt_id);
    if (s_continuous_chat || s_capture_mode == CAPTURE_MODE_CONTINUOUS) {
        stop_continuous_chat();
    }
    if (s_capture_mode == CAPTURE_MODE_PTT) {
        stop_set_capture();
    }
    if (requested_fmt && !apply_afe_diag_format(requested_fmt)) {
        app_ui_set_mic_state("FMT ERR");
        return;
    }
    if (!ensure_afe_ready()) {
        ESP_LOGW(TAG, "barge diag requires AFE/AEC");
        app_ui_set_mic_state("BARGE ERR");
        return;
    }
    const char *active_fmt = afe_capture_get_input_format();

    barge_diag_reset_state();
    s_barge_diag_prompt_id = prompt_id;
    s_barge_diag_target_ms = (uint32_t)duration_ms;
    s_barge_diag_start_tick = xTaskGetTickCount();
    s_barge_diag_end_tick = s_barge_diag_start_tick + pdMS_TO_TICKS(duration_ms);
    s_barge_diag_play_start_tick = s_barge_diag_start_tick;
    s_barge_diag_playing = true;
    s_barge_diag_active = true;
    s_barge_diag_finish_queued = false;

    afe_capture_set_raw_audio_callback(on_raw_tdm_audio, NULL);
    afe_capture_set_processed_audio_callback(on_raw_tdm_afe_audio, NULL);
    afe_capture_set_raw_channel_monitor(true);
    audio_player_set_reference_tap_callback(on_barge_reference_audio, NULL);
    app_ui_set_assistant_state(APP_UI_STATE_PLAYING);
    app_ui_set_mic_state("BARGE DIAG");
    char start_detail[160];
    snprintf(start_detail,
             sizeof(start_detail),
             "fmt=%s;prompt=%s;playback_probe=%s;capture=afe_raw_and_processed",
             active_fmt ? active_fmt : "unknown",
             barge_diag_prompt_name(prompt_id),
             barge_diag_prompt_name(prompt_id));
    mcp_client_send_diagnostic_event("barge_diag", "start", start_detail);
    ESP_LOGI(TAG,
             "barge diag start fmt=%s prompt=%s duration=%dms",
             active_fmt ? active_fmt : "unknown",
             barge_diag_prompt_name(prompt_id),
             duration_ms);

    mark_assistant_playback_busy(true);
    esp_err_t ret = play_barge_diag_prompt(prompt_id);
    audio_player_set_reference_tap_callback(NULL, NULL);
    s_barge_diag_playing = false;
    s_barge_diag_play_end_tick = xTaskGetTickCount();
    s_barge_diag_play_tail_end_tick = s_barge_diag_play_end_tick + pdMS_TO_TICKS(BARGE_DIAG_PLAYBACK_TAIL_MS);
    mark_assistant_playback_busy(false);
    ESP_LOGI(TAG,
             "barge diag playback done ret=%s playback_ms=%u",
             esp_err_to_name(ret),
             (unsigned)((s_barge_diag_play_end_tick - s_barge_diag_play_start_tick) * portTICK_PERIOD_MS));

    if (xTaskGetTickCount() >= s_barge_diag_end_tick) {
        queue_barge_diag_finish();
    } else {
        app_ui_set_assistant_state(APP_UI_STATE_RECORDING);
    }
}

static void finish_barge_diag(void)
{
    if (!s_barge_diag_active && !s_barge_diag_finish_queued) {
        return;
    }

    afe_capture_set_raw_audio_callback(NULL, NULL);
    afe_capture_set_processed_audio_callback(NULL, NULL);
    afe_capture_set_raw_channel_monitor(false);
    audio_player_set_reference_tap_callback(NULL, NULL);

    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = s_barge_diag_start_tick
                               ? (uint32_t)((now - s_barge_diag_start_tick) * portTICK_PERIOD_MS)
                               : s_barge_diag_target_ms;
    uint32_t playback_ms = (s_barge_diag_play_start_tick && s_barge_diag_play_end_tick)
                               ? (uint32_t)((s_barge_diag_play_end_tick - s_barge_diag_play_start_tick) * portTICK_PERIOD_MS)
                               : 0;
    const char *active_fmt = afe_capture_get_input_format();
    const char *prompt_name = barge_diag_prompt_name(s_barge_diag_prompt_id);
    s_barge_ref_corr = barge_ref_corr_analyze();

    ESP_LOGI(TAG,
             "barge diag done fmt=%s prompt=%s duration=%ums playback=%ums raw_channels=%d vad_start=%u vad_end=%u vad_play=%u vad_tail=%u vad_post=%u wake=%u",
             active_fmt ? active_fmt : "unknown",
             prompt_name,
             (unsigned)duration_ms,
             (unsigned)playback_ms,
             s_barge_diag_channels_seen,
             (unsigned)s_barge_diag_vad_start_count,
             (unsigned)s_barge_diag_vad_end_count,
             (unsigned)s_barge_diag_vad_start_play_count,
             (unsigned)s_barge_diag_vad_start_tail_count,
             (unsigned)s_barge_diag_vad_start_post_count,
             (unsigned)s_barge_diag_wake_count);
    ESP_LOGI(TAG,
             "barge diag ref corr best_ch=%d score=%d/1000 lag=%dms gain=%d/1000 ref_blocks=%u raw_blocks=%u",
             s_barge_ref_corr.channel,
             s_barge_ref_corr.score_permille,
             s_barge_ref_corr.lag_ms,
             s_barge_ref_corr.gain_permille,
             (unsigned)s_barge_ref_corr.ref_blocks,
             (unsigned)s_barge_ref_corr.raw_blocks);
    for (int ch = 0; ch < RAW_TDM_DIAG_CHANNELS; ++ch) {
        char name[8];
        snprintf(name, sizeof(name), "ch%d", ch);
        log_barge_diag_stats_row("play", name, &s_barge_diag_raw_play_stats[ch]);
        log_barge_diag_stats_row("post", name, &s_barge_diag_raw_post_stats[ch]);
    }
    log_barge_diag_stats_row("play", "afe", &s_barge_diag_afe_play_stats);
    log_barge_diag_stats_row("post", "afe", &s_barge_diag_afe_post_stats);

    char detail[512];
    snprintf(detail,
             sizeof(detail),
             "fmt=%s prompt=%s duration_ms=%u playback_ms=%u raw_channels=%d vad_start=%u vad_end=%u vad_during_playback=%u vad_playback_tail=%u vad_after_playback=%u wake=%u afe_play_peak=%d afe_play_avg=%u afe_post_peak=%d afe_post_avg=%u ref_best_ch=%d ref_corr=%d ref_lag_ms=%d ref_gain_permille=%d ref_blocks=%u raw_blocks=%u",
             active_fmt ? active_fmt : "unknown",
             prompt_name,
             (unsigned)duration_ms,
             (unsigned)playback_ms,
             s_barge_diag_channels_seen,
             (unsigned)s_barge_diag_vad_start_count,
             (unsigned)s_barge_diag_vad_end_count,
             (unsigned)s_barge_diag_vad_start_play_count,
             (unsigned)s_barge_diag_vad_start_tail_count,
             (unsigned)s_barge_diag_vad_start_post_count,
             (unsigned)s_barge_diag_wake_count,
             s_barge_diag_afe_play_stats.peak,
             (unsigned)diag_stats_avg(&s_barge_diag_afe_play_stats),
             s_barge_diag_afe_post_stats.peak,
             (unsigned)diag_stats_avg(&s_barge_diag_afe_post_stats),
             s_barge_ref_corr.channel,
             s_barge_ref_corr.score_permille,
             s_barge_ref_corr.lag_ms,
             s_barge_ref_corr.gain_permille,
             (unsigned)s_barge_ref_corr.ref_blocks,
             (unsigned)s_barge_ref_corr.raw_blocks);
    mcp_client_send_diagnostic_event("barge_diag", "done", detail);

    app_ui_set_assistant_state(APP_UI_STATE_IDLE);
    app_ui_set_mic_state("BARGE DONE");
    barge_diag_reset_state();
}

static void barge_diag_housekeeping(void)
{
    if (!s_barge_diag_active || s_barge_diag_finish_queued) {
        return;
    }
    if (xTaskGetTickCount() >= s_barge_diag_end_tick) {
        queue_barge_diag_finish();
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

static void mark_assistant_playback_busy(bool busy)
{
    if (s_assistant_playback_busy == busy) {
        return;
    }
    s_assistant_playback_busy = busy;
    bool canceled_by_barge = !busy && s_barge_playback_cancelled;
    if (!busy && s_continuous_chat && !canceled_by_barge) {
        TickType_t now = xTaskGetTickCount();
        s_cont_playback_tail_until_tick = now + pdMS_TO_TICKS(CONT_PLAYBACK_TAIL_IGNORE_MS);
        s_cont_playback_gate_until_tick = now + pdMS_TO_TICKS(CONT_PLAYBACK_TAIL_GATE_MS);
        s_cont_rearm_tick = now + pdMS_TO_TICKS(CONT_REARM_DELAY_MS);
        cont_vad_reset_runtime();
        cont_preroll_reset();
        ESP_LOGI(TAG,
                 "assistant playback tail gate=%d/%dms rearm=%dms",
                 CONT_PLAYBACK_TAIL_IGNORE_MS,
                 CONT_PLAYBACK_TAIL_GATE_MS,
                 CONT_REARM_DELAY_MS);
    }
    if (canceled_by_barge) {
        s_barge_playback_cancelled = false;
        ESP_LOGI(TAG, "assistant playback canceled by barge-in continuous=%d", s_continuous_chat);
    }
    ESP_LOGI(TAG, "assistant playback busy=%d continuous=%d", busy, s_continuous_chat);
}

static void on_mcp_assistant_busy(bool busy, void *ctx)
{
    (void)ctx;
    s_audio_busy = busy;
    ESP_LOGI(TAG, "assistant remote busy=%d playback=%d continuous=%d", busy, s_assistant_playback_busy, s_continuous_chat);
}

static void on_mcp_playback_busy(bool busy, void *ctx)
{
    (void)ctx;
    mark_assistant_playback_busy(busy);
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
    if (command_has_token_prefix(command, "barge_diag") ||
        command_has_token_prefix(command, "barge_in_diag") ||
        command_has_token_prefix(command, "bargein_diag") ||
        command_has_token_prefix(command, "barge")) {
        const char *space = strchr(command, ' ');
        send_cmd_nonblocking(VOICE_CMD_BARGE_DIAG, parse_barge_diag_value(space ? space + 1 : NULL));
        return;
    }
    if (command_has_token_prefix(command, "afe_vad_mute") ||
        command_has_token_prefix(command, "vad_mute_playback") ||
        command_has_token_prefix(command, "vad_mute")) {
        const char *space = strchr(command, ' ');
        int value = parse_on_off_value(space ? space + 1 : NULL);
        if (value >= 0) {
            send_cmd_nonblocking(VOICE_CMD_AFE_VAD_MUTE_SET, value);
        } else {
            ESP_LOGW(TAG, "AFE VAD mute command needs ON/OFF");
        }
        return;
    }
    if (command_has_token_prefix(command, "afe_aec_mode") ||
        command_has_token_prefix(command, "aec_mode") ||
        command_has_token_prefix(command, "fd_aec_mode")) {
        const char *space = strchr(command, ' ');
        int value = parse_afe_aec_profile_value(space ? space + 1 : NULL);
        if (value >= 0) {
            send_cmd_nonblocking(VOICE_CMD_AFE_AEC_PROFILE_SET, value);
        } else {
            ESP_LOGW(TAG, "AFE AEC mode command needs LOW/HIGH");
        }
        return;
    }
    if (command_equals(command, "mcp_url_default") || command_equals(command, "mcp_default") ||
        command_equals(command, "mcp_endpoint_default") || command_equals(command, "mcp_url_reset")) {
        send_cmd_nonblocking(VOICE_CMD_MCP_ENDPOINT_DEFAULT, 0);
        return;
    }
    if (command_equals(command, "afe_status") || command_equals(command, "afe status")) {
        send_cmd_nonblocking(VOICE_CMD_AFE_STATUS, 0);
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
    voice_cmd_t cmd = {0};

    ESP_LOGI(TAG, "idle. Hold SET to record and send to MCP.");

    while (true) {
        if (s_loop_enabled) {
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) {
                cmd.type = VOICE_CMD_PLAY_ONCE;
            }
        } else if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(CONT_VAD_WATCHDOG_MS)) != pdTRUE) {
            raw_tdm_diag_housekeeping();
            barge_diag_housekeeping();
            continuous_chat_housekeeping();
            continue;
        }

        switch (cmd.type) {
            case VOICE_CMD_PLAY_ONCE:
                mark_assistant_playback_busy(true);
                app_ui_set_assistant_state(APP_UI_STATE_PLAYING);
                audio_player_play_xiaole();
                app_ui_set_assistant_state(APP_UI_STATE_IDLE);
                mark_assistant_playback_busy(false);
                if (s_loop_enabled) {
                    vTaskDelay(pdMS_TO_TICKS(VOICE_LOOP_GAP_MS));
                }
                break;
            case VOICE_CMD_LOOP:
                s_loop_enabled = true;
                ESP_LOGI(TAG, "loop enabled");
                app_ui_set_voice_state("VOICE LOOP");
                break;
            case VOICE_CMD_STOP:
                s_loop_enabled = false;
                stop_continuous_chat();
                stop_set_capture();
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
            case VOICE_CMD_MCP_ENDPOINT_DEFAULT:
                mcp_client_reset_endpoint_to_default();
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
            case VOICE_CMD_BARGE_DIAG:
                start_barge_diag(cmd.value);
                break;
            case VOICE_CMD_BARGE_DIAG_DONE:
                finish_barge_diag();
                break;
            case VOICE_CMD_AFE_VAD_MUTE_SET:
                apply_afe_vad_mute_playback(cmd.value != 0);
                break;
            case VOICE_CMD_AFE_AEC_PROFILE_SET:
                apply_afe_aec_profile((afe_capture_aec_profile_t)cmd.value);
                break;
            case VOICE_CMD_AFE_STATUS:
                log_afe_status();
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
        } else if (strcmp(line, "BARGE") == 0 || strcmp(line, "BARGE DIAG") == 0) {
            send_cmd(VOICE_CMD_BARGE_DIAG, parse_barge_diag_value(NULL));
        } else if (strncmp(line, "BARGE DIAG ", 11) == 0) {
            send_cmd(VOICE_CMD_BARGE_DIAG, parse_barge_diag_value(line + 11));
        } else if (strncmp(line, "BARGE ", 6) == 0) {
            send_cmd(VOICE_CMD_BARGE_DIAG, parse_barge_diag_value(line + 6));
        } else if (strncmp(line, "AFE VAD MUTE ", 13) == 0) {
            int value = parse_on_off_value(raw_line + 13);
            if (value >= 0) {
                send_cmd(VOICE_CMD_AFE_VAD_MUTE_SET, value);
            } else {
                ESP_LOGW(TAG, "usage: AFE VAD MUTE ON/OFF");
            }
        } else if (strncmp(line, "VAD MUTE ", 9) == 0) {
            int value = parse_on_off_value(raw_line + 9);
            if (value >= 0) {
                send_cmd(VOICE_CMD_AFE_VAD_MUTE_SET, value);
            } else {
                ESP_LOGW(TAG, "usage: VAD MUTE ON/OFF");
            }
        } else if (strcmp(line, "AFE STATUS") == 0 || strcmp(line, "AEC STATUS") == 0) {
            send_cmd(VOICE_CMD_AFE_STATUS, 0);
        } else if (strncmp(line, "AFE AEC MODE ", 13) == 0) {
            int value = parse_afe_aec_profile_value(raw_line + 13);
            if (value >= 0) {
                send_cmd(VOICE_CMD_AFE_AEC_PROFILE_SET, value);
            } else {
                ESP_LOGW(TAG, "usage: AFE AEC MODE LOW/HIGH");
            }
        } else if (strncmp(line, "AEC MODE ", 9) == 0) {
            int value = parse_afe_aec_profile_value(raw_line + 9);
            if (value >= 0) {
                send_cmd(VOICE_CMD_AFE_AEC_PROFILE_SET, value);
            } else {
                ESP_LOGW(TAG, "usage: AEC MODE LOW/HIGH");
            }
        } else if (strncmp(line, "AEC ", 4) == 0) {
            int value = parse_afe_aec_profile_value(raw_line + 4);
            if (value >= 0) {
                send_cmd(VOICE_CMD_AFE_AEC_PROFILE_SET, value);
            } else {
                ESP_LOGW(TAG, "usage: AEC LOW/HIGH");
            }
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
        } else if (strcmp(line, "MCP URL DEFAULT") == 0 || strcmp(line, "MCP DEFAULT") == 0 ||
                   strcmp(line, "MCP URL RESET") == 0 || strcmp(line, "MCP RESET") == 0) {
            send_cmd(VOICE_CMD_MCP_ENDPOINT_DEFAULT, 0);
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
    mcp_client_set_playback_callback(on_mcp_playback_busy, NULL);
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

    xTaskCreate(playback_task, "voice_playback", 6144, NULL, 5, NULL);
    xTaskCreate(command_task, "voice_command", 4096, NULL, 4, NULL);

    if (app_buttons_init(on_button_event, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button init failed");
    }

    if (!mcp_client_is_connected()) {
        send_cmd(VOICE_CMD_MCP_CONNECT, 0);
    }
}
