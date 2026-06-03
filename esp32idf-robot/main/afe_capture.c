#include "afe_capture.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "audio_element.h"
#include "audio_hal.h"
#include "audio_pipeline.h"
#include "board.h"
#include "driver/i2s.h"
#ifdef CONFIG_ESP32_S3_KORVO2_V3_BOARD
#include "es7210.h"
#endif
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vadn_models.h"
#include "esp_wn_models.h"
#include "filter_resample.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "sdkconfig.h"

#define AFE_CAPTURE_RATE 16000
#define AFE_CAPTURE_BITS 16
#define AFE_CAPTURE_FEED_TASK_STACK (4 * 1024)
#define AFE_CAPTURE_FETCH_TASK_STACK (5 * 1024)
#define AFE_CAPTURE_FEED_TASK_PRIO 6
#define AFE_CAPTURE_FETCH_TASK_PRIO 5
#define AFE_CAPTURE_FEED_TASK_CORE 1
#define AFE_CAPTURE_FETCH_TASK_CORE 0
#define AFE_CAPTURE_AFE_TASK_PRIO 5
#define AFE_CAPTURE_AFE_RINGBUF_FRAMES 16
#define AFE_CAPTURE_VAD_OFF_MS 850
#define AFE_CAPTURE_VAD_START_MS 160
#define AFE_CAPTURE_VAD_DELAY_MS 128
#define AFE_CAPTURE_RNNM_VAD_OFF_MS 900
#define AFE_CAPTURE_RNNM_VAD_START_MS 180
#define AFE_CAPTURE_RNNM_VAD_DELAY_MS 160
#define AFE_CAPTURE_LOG_INTERVAL_MS 1000
#define AFE_CAPTURE_I2S_TIMEOUT_MS 100
#define AFE_CAPTURE_AEC_FILTER_LENGTH 4
#define AFE_CAPTURE_AFE_TYPE AFE_TYPE_FD
#define AFE_CAPTURE_AFE_MODE AFE_MODE_LOW_COST
#define AFE_CAPTURE_DEFAULT_AEC_PROFILE AFE_CAPTURE_AEC_PROFILE_FD_LOW_COST
#define AFE_CAPTURE_AEC_NLP_LEVEL AEC_NLP_LEVEL_AGGR
#define AFE_CAPTURE_ESP_SR_FD_MIN_VERSION "2.4.3"
#define AFE_CAPTURE_ESP_SR_PINNED_VERSION "2.4.6"
#define AFE_CAPTURE_OUTPUT_PLAYBACK_CHANNEL 0
#define AFE_CAPTURE_FIXED_OUTPUT_CHANNEL 1
#define AFE_CAPTURE_FIXED_FIRST_CHANNEL 1
#ifndef ROBOT_ALLOW_EXPERIMENTAL_AEC_HIGH_PERF
#define ROBOT_ALLOW_EXPERIMENTAL_AEC_HIGH_PERF 0
#endif
#define AFE_CAPTURE_I2S_OUT_RB_SIZE (2 * 1024)
#define AFE_CAPTURE_RAW_OUT_RB_SIZE (4 * 1024)
#define AFE_CAPTURE_FILTER_OUT_RB_SIZE (2 * 1024)
#define AFE_CAPTURE_RAW_MONITOR_CHANNELS 4
#define AFE_CAPTURE_INPUT_FMT_MAX_LEN 7

#if defined(CONFIG_ESP32_S3_KORVO2_V3_BOARD)
#define AFE_CAPTURE_DEFAULT_INPUT_FMT "RNNM"
#define AFE_CAPTURE_I2S_RATE AFE_CAPTURE_RATE
#define AFE_CAPTURE_I2S_BITS I2S_DATA_BIT_WIDTH_32BIT
#define AFE_CAPTURE_I2S_CHANNEL_TYPE I2S_CHANNEL_FMT_RIGHT_LEFT
#define AFE_CAPTURE_USE_RESAMPLE_FILTER 0
#define AFE_CAPTURE_FILTER_SRC_CH 0
#define AFE_CAPTURE_FILTER_DEST_CH 0
#else
#define AFE_CAPTURE_DEFAULT_INPUT_FMT AUDIO_ADC_INPUT_CH_FORMAT
#define AFE_CAPTURE_I2S_RATE AFE_CAPTURE_RATE
#define AFE_CAPTURE_I2S_BITS CODEC_ADC_BITS_PER_SAMPLE
#define AFE_CAPTURE_I2S_CHANNEL_TYPE I2S_CHANNEL_FMT_RIGHT_LEFT
#define AFE_CAPTURE_USE_RESAMPLE_FILTER 0
#define AFE_CAPTURE_FILTER_SRC_CH 0
#define AFE_CAPTURE_FILTER_DEST_CH 0
#endif

static const char *TAG = "AFE_CAPTURE";

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_i2s_reader;
static audio_element_handle_t s_filter;
static audio_element_handle_t s_raw_reader;
static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;
static afe_capture_audio_cb_t s_audio_cb;
static void *s_audio_ctx;
static afe_capture_event_cb_t s_event_cb;
static void *s_event_ctx;
static afe_capture_raw_audio_cb_t s_raw_audio_cb;
static void *s_raw_audio_ctx;
static afe_capture_processed_audio_cb_t s_processed_audio_cb;
static void *s_processed_audio_ctx;
static volatile bool s_running;
static volatile bool s_tasks_exit;
static bool s_pipeline_running;
static bool s_initialized;
static bool s_wake_enabled;
static bool s_has_wake_model;
static bool s_vad_speech;
static bool s_wake_latched;
static bool s_has_ref_channel;
static afe_capture_aec_profile_t s_aec_profile = AFE_CAPTURE_DEFAULT_AEC_PROFILE;
static bool s_vad_mute_playback;
static TickType_t s_capture_start_tick;
static volatile uint32_t s_capture_bytes;
static TickType_t s_feed_wait_log_tick;
static TickType_t s_fetch_log_tick;
static TickType_t s_raw_monitor_log_tick;
static bool s_raw_monitor_enabled;
static int s_feed_chunk_samples;
static int s_fetch_chunk_samples;
static int s_feed_channels;
static int s_fetch_channels;
static int s_feed_bytes;
static char s_input_format[AFE_CAPTURE_INPUT_FMT_MAX_LEN + 1] = AFE_CAPTURE_DEFAULT_INPUT_FMT;

static int abs16_local(int16_t value)
{
    return value < 0 ? -value : value;
}

static void *capture_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static bool aec_profile_is_valid(afe_capture_aec_profile_t profile)
{
    if (profile == AFE_CAPTURE_AEC_PROFILE_FD_LOW_COST) {
        return true;
    }
#if ROBOT_ALLOW_EXPERIMENTAL_AEC_HIGH_PERF
    if (profile == AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF) {
        return true;
    }
#endif
    return false;
}

static aec_mode_t resolve_aec_mode(afe_capture_aec_profile_t profile)
{
    return profile == AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF
               ? AEC_MODE_FD_HIGH_PERF
               : AEC_MODE_FD_LOW_COST;
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG,
             "%s heap internal=%u largest=%u psram=%u psram_largest=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static bool input_format_has_ref(const char *fmt)
{
    return fmt && strchr(fmt, 'R') != NULL;
}

static int input_format_count_channel(const char *fmt, char channel)
{
    if (!fmt) {
        return 0;
    }
    int count = 0;
    char wanted = (char)toupper((unsigned char)channel);
    for (const char *p = fmt; *p; ++p) {
        if (toupper((unsigned char)*p) == wanted) {
            ++count;
        }
    }
    return count;
}

static bool input_format_is_supported(const char *fmt)
{
    if (!fmt || !fmt[0]) {
        return false;
    }
    size_t len = strlen(fmt);
    if (len == 0 || len > AFE_CAPTURE_INPUT_FMT_MAX_LEN) {
        return false;
    }
#if defined(CONFIG_ESP32_S3_KORVO2_V3_BOARD)
    if (len != 4) {
        return false;
    }
#endif
    bool has_mic = false;
    for (size_t i = 0; i < len; ++i) {
        char ch = (char)toupper((unsigned char)fmt[i]);
        if (ch == 'M') {
            has_mic = true;
        } else if (ch != 'R' && ch != 'N') {
            return false;
        }
    }
    return has_mic;
}

static esp_err_t normalize_input_format(const char *fmt, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !input_format_is_supported(fmt)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(fmt);
    if (len >= out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = (char)toupper((unsigned char)fmt[i]);
    }
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t validate_official_fd_afe_config(const afe_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->afe_type != AFE_TYPE_FD) {
        ESP_LOGE(TAG, "official full-duplex AFE requires AFE_TYPE_FD, got %d", cfg->afe_type);
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg->aec_init || cfg->pcm_config.ref_num <= 0) {
        ESP_LOGE(TAG,
                 "official full-duplex AEC requires an active playback reference channel, input=%s ref_num=%d",
                 s_input_format,
                 cfg->pcm_config.ref_num);
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->aec_mode != AEC_MODE_FD_LOW_COST && cfg->aec_mode != AEC_MODE_FD_HIGH_PERF) {
        ESP_LOGE(TAG, "official full-duplex AEC requires FD AEC mode, got %d", cfg->aec_mode);
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->output_playback_channel) {
        ESP_LOGE(TAG, "AFE fetch output must not include playback reference for Doubao upload");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg->fixed_output_channel || !cfg->fixed_first_channel) {
        ESP_LOGE(TAG,
                 "AFE fetch output must stay fixed to the selected microphone path, fixed_output=%d fixed_first=%d",
                 cfg->fixed_output_channel,
                 cfg->fixed_first_channel);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void log_official_fd_afe_config(const afe_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    ESP_LOGI(TAG,
             "esp-sr fd-aec contract: min_version=%s pinned=%s api=AFE_TYPE_FD/AEC_MODE_FD_*",
             AFE_CAPTURE_ESP_SR_FD_MIN_VERSION,
             AFE_CAPTURE_ESP_SR_PINNED_VERSION);
    ESP_LOGI(TAG,
             "fd-aec input=%s fmt_mic=%d fmt_ref=%d pcm_total=%d pcm_mic=%d pcm_ref=%d sample_rate=%d",
             s_input_format,
             input_format_count_channel(s_input_format, 'M'),
             input_format_count_channel(s_input_format, 'R'),
             cfg->pcm_config.total_ch_num,
             cfg->pcm_config.mic_num,
             cfg->pcm_config.ref_num,
             cfg->pcm_config.sample_rate);
    ESP_LOGI(TAG,
             "fd-aec modes afe_type=%d afe_mode=%d aec_init=%d aec_mode=%d filter=%d nlp=%d ns=%d fixed_first=%d fixed_output=%d output_ref=%d",
             cfg->afe_type,
             cfg->afe_mode,
             cfg->aec_init,
             cfg->aec_mode,
             cfg->aec_filter_length,
             cfg->aec_nlp_level,
             cfg->ns_init,
             cfg->fixed_first_channel,
             cfg->fixed_output_channel,
             cfg->output_playback_channel);
}

static void analyze_level(const int16_t *samples, int bytes, int *peak, int *avg_abs)
{
    int sample_count = bytes / (int)sizeof(int16_t);
    int max_peak = 0;
    int64_t sum_abs = 0;

    for (int i = 0; i < sample_count; ++i) {
        int level = abs16_local(samples[i]);
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

static uint32_t isqrt_u64(uint64_t value)
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

static void log_raw_channel_levels(const int16_t *samples, int frames, int channels)
{
    if (!s_raw_monitor_enabled || !samples || frames <= 0 || channels <= 0) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((uint32_t)((now - s_raw_monitor_log_tick) * portTICK_PERIOD_MS) < AFE_CAPTURE_LOG_INTERVAL_MS) {
        return;
    }
    s_raw_monitor_log_tick = now;

    int channel_count = channels < AFE_CAPTURE_RAW_MONITOR_CHANNELS ? channels : AFE_CAPTURE_RAW_MONITOR_CHANNELS;
    int peak[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    int64_t sum_abs[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    uint64_t sum_sq[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    int zc[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    int16_t prev[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    bool prev_valid[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {false};

    for (int frame = 0; frame < frames; ++frame) {
        const int16_t *row = samples + frame * channels;
        for (int ch = 0; ch < channel_count; ++ch) {
            int16_t sample = row[ch];
            int level = abs16_local(sample);
            if (level > peak[ch]) {
                peak[ch] = level;
            }
            sum_abs[ch] += level;
            sum_sq[ch] += (uint64_t)((int32_t)sample * (int32_t)sample);
            if (prev_valid[ch] && ((sample < 0) != (prev[ch] < 0))) {
                ++zc[ch];
            }
            prev[ch] = sample;
            prev_valid[ch] = true;
        }
    }

    int avg[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    uint32_t rms[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    int zcr_pm[AFE_CAPTURE_RAW_MONITOR_CHANNELS] = {0};
    for (int ch = 0; ch < channel_count; ++ch) {
        avg[ch] = (int)(sum_abs[ch] / frames);
        rms[ch] = isqrt_u64(sum_sq[ch] / (uint64_t)frames);
        zcr_pm[ch] = frames > 1 ? (zc[ch] * 1000) / (frames - 1) : 0;
    }

    if (channel_count >= 4) {
        ESP_LOGI(TAG,
                 "raw tdm frames=%d ch0 peak=%d avg=%d rms=%u zcr=%d ch1 peak=%d avg=%d rms=%u zcr=%d ch2 peak=%d avg=%d rms=%u zcr=%d ch3 peak=%d avg=%d rms=%u zcr=%d",
                 frames,
                 peak[0],
                 avg[0],
                 (unsigned)rms[0],
                 zcr_pm[0],
                 peak[1],
                 avg[1],
                 (unsigned)rms[1],
                 zcr_pm[1],
                 peak[2],
                 avg[2],
                 (unsigned)rms[2],
                 zcr_pm[2],
                 peak[3],
                 avg[3],
                 (unsigned)rms[3],
                 zcr_pm[3]);
    } else {
        ESP_LOGI(TAG,
                 "raw tdm frames=%d channels=%d ch0 peak=%d avg=%d rms=%u zcr=%d ch1 peak=%d avg=%d rms=%u zcr=%d",
                 frames,
                 channels,
                 peak[0],
                 avg[0],
                 (unsigned)rms[0],
                 zcr_pm[0],
                 channel_count > 1 ? peak[1] : 0,
                 channel_count > 1 ? avg[1] : 0,
                 channel_count > 1 ? (unsigned)rms[1] : 0,
                 channel_count > 1 ? zcr_pm[1] : 0);
    }
}

static void emit_event(afe_capture_event_t event)
{
    if (s_event_cb) {
        s_event_cb(event, s_event_ctx);
    }
}

static char *find_model_by_prefix(srmodel_list_t *models, const char *prefix, char *skip)
{
    if (!models || !prefix) {
        return NULL;
    }
    for (int i = 0; i < models->num; ++i) {
        char *name = models->model_name[i];
        if (!name || name == skip) {
            continue;
        }
        if (strstr(name, prefix)) {
            return name;
        }
    }
    return NULL;
}

static void apply_wakenet_runtime_state(void)
{
    if (!s_initialized || !s_afe_handle || !s_afe_data || !s_has_wake_model) {
        return;
    }

    int ret = s_wake_enabled
                  ? s_afe_handle->enable_wakenet(s_afe_data)
                  : s_afe_handle->disable_wakenet(s_afe_data);
    if (ret < 0) {
        ESP_LOGW(TAG, "wakenet %s failed ret=%d", s_wake_enabled ? "enable" : "disable", ret);
    }
    s_wake_latched = false;
}

static void cleanup_tasks(void)
{
    s_tasks_exit = true;
    for (int i = 0; (s_feed_task || s_fetch_task) && i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void cleanup_audio_pipeline(void)
{
    if (s_pipeline) {
        if (s_pipeline_running) {
            audio_pipeline_stop(s_pipeline);
            audio_pipeline_wait_for_stop(s_pipeline);
            audio_pipeline_terminate(s_pipeline);
            s_pipeline_running = false;
        }
        if (s_i2s_reader) {
            audio_pipeline_unregister(s_pipeline, s_i2s_reader);
        }
        if (s_filter) {
            audio_pipeline_unregister(s_pipeline, s_filter);
        }
        if (s_raw_reader) {
            audio_pipeline_unregister(s_pipeline, s_raw_reader);
        }
        audio_pipeline_deinit(s_pipeline);
        s_pipeline = NULL;
    }
    if (s_i2s_reader) {
        audio_element_deinit(s_i2s_reader);
        s_i2s_reader = NULL;
    }
    if (s_filter) {
        audio_element_deinit(s_filter);
        s_filter = NULL;
    }
    if (s_raw_reader) {
        audio_element_deinit(s_raw_reader);
        s_raw_reader = NULL;
    }
}

static esp_err_t create_audio_pipeline(void)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipeline) {
        ESP_LOGE(TAG, "audio_pipeline_init failed");
        return ESP_FAIL;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT,
                                                                AFE_CAPTURE_I2S_RATE,
                                                                AFE_CAPTURE_I2S_BITS,
                                                                AUDIO_STREAM_READER);
    i2s_cfg.stack_in_ext = true;
    i2s_cfg.out_rb_size = AFE_CAPTURE_I2S_OUT_RB_SIZE;
    i2s_cfg.chan_cfg.dma_desc_num = 4;
    i2s_cfg.chan_cfg.dma_frame_num = 160;
    i2s_stream_set_channel_type(&i2s_cfg, AFE_CAPTURE_I2S_CHANNEL_TYPE);
    s_i2s_reader = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_reader) {
        ESP_LOGE(TAG, "i2s_stream_init failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
    audio_element_set_input_timeout(s_i2s_reader, pdMS_TO_TICKS(AFE_CAPTURE_I2S_TIMEOUT_MS));

#if AFE_CAPTURE_USE_RESAMPLE_FILTER
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = AFE_CAPTURE_I2S_RATE;
    rsp_cfg.dest_rate = AFE_CAPTURE_RATE;
    rsp_cfg.mode = RESAMPLE_UNCROSS_MODE;
    rsp_cfg.src_ch = AFE_CAPTURE_FILTER_SRC_CH;
    rsp_cfg.dest_ch = AFE_CAPTURE_FILTER_DEST_CH;
    rsp_cfg.complexity = 1;
    rsp_cfg.max_indata_bytes = 1024;
    rsp_cfg.out_rb_size = AFE_CAPTURE_FILTER_OUT_RB_SIZE;
    s_filter = rsp_filter_init(&rsp_cfg);
    if (!s_filter) {
        ESP_LOGE(TAG, "rsp_filter_init failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
#endif

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = AFE_CAPTURE_RAW_OUT_RB_SIZE;
    s_raw_reader = raw_stream_init(&raw_cfg);
    if (!s_raw_reader) {
        ESP_LOGE(TAG, "raw_stream_init failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }

    if (audio_pipeline_register(s_pipeline, s_i2s_reader, "i2s") != ESP_OK) {
        ESP_LOGE(TAG, "register i2s failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
#if AFE_CAPTURE_USE_RESAMPLE_FILTER
    if (audio_pipeline_register(s_pipeline, s_filter, "filter") != ESP_OK ||
        audio_pipeline_register(s_pipeline, s_raw_reader, "raw") != ESP_OK) {
        ESP_LOGE(TAG, "register filter/raw failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
    const char *link_tag[3] = {"i2s", "filter", "raw"};
    if (audio_pipeline_link(s_pipeline, &link_tag[0], 3) != ESP_OK) {
        ESP_LOGE(TAG, "link i2s/filter/raw failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
#else
    if (audio_pipeline_register(s_pipeline, s_raw_reader, "raw") != ESP_OK) {
        ESP_LOGE(TAG, "register raw failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
    const char *link_tag[2] = {"i2s", "raw"};
    if (audio_pipeline_link(s_pipeline, &link_tag[0], 2) != ESP_OK) {
        ESP_LOGE(TAG, "link i2s/raw failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
#endif

    if (audio_pipeline_run(s_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        cleanup_audio_pipeline();
        return ESP_FAIL;
    }
    s_pipeline_running = true;
    ESP_LOGI(TAG,
             "audio pipeline ready: i2s=%dHz bits=%d input=%s filter=%d raw=%dHz",
             AFE_CAPTURE_I2S_RATE,
             (int)AFE_CAPTURE_I2S_BITS,
             s_input_format,
             AFE_CAPTURE_USE_RESAMPLE_FILTER,
             AFE_CAPTURE_RATE);
    return ESP_OK;
}

static void afe_capture_cleanup_failed_init(void)
{
    cleanup_tasks();

    if (s_afe_handle && s_afe_data) {
        s_afe_handle->destroy(s_afe_data);
    }
    s_afe_data = NULL;
    s_afe_handle = NULL;

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }

    cleanup_audio_pipeline();

    s_initialized = false;
    s_has_wake_model = false;
    s_vad_speech = false;
    s_wake_latched = false;
}

static int read_i2s_exact(uint8_t *buf, int wanted)
{
    int filled = 0;
    while (!s_tasks_exit && filled < wanted) {
        int ret = raw_stream_read(s_raw_reader, (char *)buf + filled, wanted - filled);
        if (ret > 0) {
            filled += ret;
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if ((uint32_t)((now - s_feed_wait_log_tick) * portTICK_PERIOD_MS) >= AFE_CAPTURE_LOG_INTERVAL_MS) {
            s_feed_wait_log_tick = now;
            ESP_LOGW(TAG, "raw feed wait ret=%d filled=%d/%d", ret, filled, wanted);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return s_tasks_exit ? AEL_IO_ABORT : filled;
}

static void forward_audio(const int16_t *data, int bytes)
{
    if (!s_running || !s_audio_cb || !data || bytes <= 0) {
        return;
    }
    s_capture_bytes += (uint32_t)bytes;
    s_audio_cb((const uint8_t *)data, bytes, s_audio_ctx);
}

static void handle_fetch_result(afe_fetch_result_t *result)
{
    if (!result) {
        return;
    }
    if (result->ret_value != ESP_OK) {
        TickType_t now = xTaskGetTickCount();
        if ((uint32_t)((now - s_fetch_log_tick) * portTICK_PERIOD_MS) >= AFE_CAPTURE_LOG_INTERVAL_MS) {
            s_fetch_log_tick = now;
            ESP_LOGW(TAG, "afe fetch wait ret=%d rb_free=%.2f running=%d",
                     result->ret_value,
                     (double)result->ringbuff_free_pct,
                     s_running);
        }
        return;
    }

    bool speech = result->vad_state == VAD_SPEECH;
    bool wake = result->wakeup_state == WAKENET_DETECTED ||
                result->wakeup_state == WAKENET_CHANNEL_VERIFIED;
    bool should_log_activity = s_running || s_raw_monitor_enabled || s_processed_audio_cb != NULL;

    if (wake && s_wake_enabled && !s_wake_latched) {
        s_wake_latched = true;
        ESP_LOGI(TAG,
                 "wakenet wake state=%d word=%d model=%d channel=%d volume=%.1f",
                 result->wakeup_state,
                 result->wake_word_index,
                 result->wakenet_model_index,
                 result->trigger_channel_id,
                 (double)result->data_volume);
        emit_event(AFE_CAPTURE_EVENT_WAKE);
    } else if (!wake) {
        s_wake_latched = false;
    }

    if (speech && !s_vad_speech) {
        s_vad_speech = true;
        if (should_log_activity) {
            ESP_LOGI(TAG, "afe vad start volume=%.1f cache=%d", (double)result->data_volume, result->vad_cache_size);
        }
        emit_event(AFE_CAPTURE_EVENT_VAD_START);
        forward_audio(result->vad_cache, result->vad_cache_size);
    }

    if (result->data && result->data_size > 0) {
        TickType_t now = xTaskGetTickCount();
        bool should_log_fetch = s_running || s_raw_monitor_enabled || s_processed_audio_cb != NULL;
        if (should_log_fetch &&
            (uint32_t)((now - s_fetch_log_tick) * portTICK_PERIOD_MS) >= AFE_CAPTURE_LOG_INTERVAL_MS) {
            int peak = 0;
            int avg_abs = 0;
            analyze_level(result->data, result->data_size, &peak, &avg_abs);
            s_fetch_log_tick = now;
            ESP_LOGI(TAG,
                     "afe fetch bytes=%d peak=%d avg=%d vad=%d wake=%d rb_free=%.2f running=%d",
                     result->data_size,
                     peak,
                     avg_abs,
                     result->vad_state,
                     result->wakeup_state,
                     (double)result->ringbuff_free_pct,
                     s_running);
        }
        afe_capture_processed_audio_cb_t processed_cb = s_processed_audio_cb;
        if (processed_cb) {
            processed_cb((const uint8_t *)result->data, result->data_size, s_processed_audio_ctx);
        }
        forward_audio(result->data, result->data_size);
    }

    if (!speech && s_vad_speech) {
        s_vad_speech = false;
        if (should_log_activity) {
            ESP_LOGI(TAG, "afe vad end volume=%.1f", (double)result->data_volume);
        }
        emit_event(AFE_CAPTURE_EVENT_VAD_END);
    }
}

static void afe_capture_feed_task(void *arg)
{
    (void)arg;
    uint8_t *buf = (uint8_t *)capture_malloc((size_t)s_feed_bytes);
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate feed buffer bytes=%d", s_feed_bytes);
        log_heap("feed buffer alloc failed");
        emit_event(AFE_CAPTURE_EVENT_ERROR);
        s_feed_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed task ready bytes=%d samples=%d channels=%d", s_feed_bytes, s_feed_chunk_samples, s_feed_channels);
    while (!s_tasks_exit) {
        int ret = read_i2s_exact(buf, s_feed_bytes);
        if (ret == s_feed_bytes && s_afe_handle && s_afe_data) {
            int frames = s_feed_channels > 0 ? ret / ((int)sizeof(int16_t) * s_feed_channels) : 0;
            if (frames > 0) {
                const int16_t *interleaved = (const int16_t *)buf;
                log_raw_channel_levels(interleaved, frames, s_feed_channels);
                afe_capture_raw_audio_cb_t raw_cb = s_raw_audio_cb;
                if (raw_cb) {
                    raw_cb(interleaved, frames, s_feed_channels, s_raw_audio_ctx);
                }
            }
            int fed = s_afe_handle->feed(s_afe_data, (const int16_t *)buf);
            if (fed <= 0) {
                ESP_LOGW(TAG, "afe feed ret=%d", fed);
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else if (ret != AEL_IO_ABORT) {
            ESP_LOGW(TAG, "i2s short read ret=%d expected=%d", ret, s_feed_bytes);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    free(buf);
    s_feed_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void afe_capture_fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "fetch task ready samples=%d channels=%d", s_fetch_chunk_samples, s_fetch_channels);
    while (!s_tasks_exit) {
        if (!s_afe_handle || !s_afe_data) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        afe_fetch_result_t *result = s_afe_handle->fetch(s_afe_data);
        if (!result) {
            continue;
        }
        handle_fetch_result(result);
    }

    s_fetch_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t create_afe_tasks(void)
{
    s_tasks_exit = false;

    BaseType_t feed_ok = xTaskCreatePinnedToCoreWithCaps(afe_capture_feed_task,
                                                         "afe_feed",
                                                         AFE_CAPTURE_FEED_TASK_STACK,
                                                         NULL,
                                                         AFE_CAPTURE_FEED_TASK_PRIO,
                                                         &s_feed_task,
                                                         AFE_CAPTURE_FEED_TASK_CORE,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (feed_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create feed task");
        log_heap("feed task create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t fetch_ok = xTaskCreatePinnedToCoreWithCaps(afe_capture_fetch_task,
                                                          "afe_fetch",
                                                          AFE_CAPTURE_FETCH_TASK_STACK,
                                                          NULL,
                                                          AFE_CAPTURE_FETCH_TASK_PRIO,
                                                          &s_fetch_task,
                                                          AFE_CAPTURE_FETCH_TASK_CORE,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fetch_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create fetch task");
        log_heap("fetch task create failed");
        cleanup_tasks();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void afe_capture_deinit_afe_runtime(void)
{
    s_running = false;
    s_audio_cb = NULL;
    s_audio_ctx = NULL;
    s_raw_audio_cb = NULL;
    s_raw_audio_ctx = NULL;
    s_processed_audio_cb = NULL;
    s_processed_audio_ctx = NULL;
    s_raw_monitor_enabled = false;

    cleanup_tasks();

    if (s_afe_handle && s_afe_data) {
        s_afe_handle->destroy(s_afe_data);
    }
    s_afe_data = NULL;
    s_afe_handle = NULL;
    s_initialized = false;
    s_has_wake_model = false;
    s_vad_speech = false;
    s_wake_latched = false;
    s_has_ref_channel = false;
    s_capture_bytes = 0;
    s_feed_wait_log_tick = 0;
    s_fetch_log_tick = 0;
    s_raw_monitor_log_tick = 0;
    s_feed_chunk_samples = 0;
    s_fetch_chunk_samples = 0;
    s_feed_channels = 0;
    s_fetch_channels = 0;
    s_feed_bytes = 0;
}

esp_err_t afe_capture_init(afe_capture_event_cb_t event_cb, void *event_ctx)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_event_cb = event_cb;
    s_event_ctx = event_ctx;
    s_has_ref_channel = input_format_has_ref(s_input_format);
    if (!s_has_ref_channel) {
        ESP_LOGE(TAG,
                 "AFE init rejected: official full-duplex AEC needs an R playback reference channel, input=%s",
                 s_input_format);
        return ESP_ERR_INVALID_ARG;
    }
    log_heap("init begin");

    audio_board_handle_t board = audio_board_get_handle();
    if (!board) {
        board = audio_board_init();
    }
    if (!board) {
        ESP_LOGE(TAG, "audio board not available");
        return ESP_FAIL;
    }
    if (board->adc_hal) {
        audio_hal_ctrl_codec(board->adc_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    } else if (board->audio_hal) {
        audio_hal_ctrl_codec(board->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    }
#ifdef CONFIG_ESP32_S3_KORVO2_V3_BOARD
    es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3);
    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_30DB);
    es7210_adc_set_gain(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2, GAIN_33DB);
#endif

    if (!s_pipeline || !s_raw_reader) {
        if (create_audio_pipeline() != ESP_OK) {
            afe_capture_cleanup_failed_init();
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG,
                 "audio pipeline reused: i2s=%dHz bits=%d input=%s filter=%d raw=%dHz",
                 AFE_CAPTURE_I2S_RATE,
                 (int)AFE_CAPTURE_I2S_BITS,
                 s_input_format,
                 AFE_CAPTURE_USE_RESAMPLE_FILTER,
                 AFE_CAPTURE_RATE);
    }
    log_heap("after audio pipeline");

    if (!s_models) {
        s_models = esp_srmodel_init("model");
    }
    if (!s_models || s_models->num == 0) {
        ESP_LOGW(TAG, "no ESP-SR model found in partition 'model'; WakeNet/VADNet disabled");
    } else {
        ESP_LOGI(TAG, "loaded %d ESP-SR model(s)", s_models->num);
    }
    log_heap("after model load");

    afe_config_t *afe_cfg = afe_config_init(s_input_format, s_models, AFE_CAPTURE_AFE_TYPE, AFE_CAPTURE_AFE_MODE);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        afe_capture_cleanup_failed_init();
        return ESP_FAIL;
    }

    char *wn_model = find_model_by_prefix(s_models, ESP_WN_PREFIX, NULL);
    char *wn_model_2 = find_model_by_prefix(s_models, ESP_WN_PREFIX, wn_model);
    char *vad_model = NULL;
#ifdef CONFIG_SR_VADN_VADNET1_MEDIUM
    vad_model = find_model_by_prefix(s_models, ESP_VADN_PREFIX, NULL);
#endif

    bool init_wakenet = wn_model != NULL;
    bool rnnm_tuned = strcmp(s_input_format, "RNNM") == 0;
    int vad_min_speech_ms = rnnm_tuned ? AFE_CAPTURE_RNNM_VAD_START_MS : AFE_CAPTURE_VAD_START_MS;
    int vad_min_noise_ms = rnnm_tuned ? AFE_CAPTURE_RNNM_VAD_OFF_MS : AFE_CAPTURE_VAD_OFF_MS;
    int vad_delay_ms = rnnm_tuned ? AFE_CAPTURE_RNNM_VAD_DELAY_MS : AFE_CAPTURE_VAD_DELAY_MS;
    s_has_wake_model = init_wakenet;
    afe_cfg->aec_init = s_has_ref_channel;
    afe_cfg->aec_mode = resolve_aec_mode(s_aec_profile);
    afe_cfg->aec_filter_length = AFE_CAPTURE_AEC_FILTER_LENGTH;
    afe_cfg->aec_nlp_level = AFE_CAPTURE_AEC_NLP_LEVEL;
    afe_cfg->se_init = false;
    afe_cfg->ns_init = true;
    afe_cfg->vad_init = true;
    afe_cfg->vad_mode = VAD_MODE_2;
    afe_cfg->vad_model_name = vad_model;
    afe_cfg->vad_min_speech_ms = vad_min_speech_ms;
    afe_cfg->vad_min_noise_ms = vad_min_noise_ms;
    afe_cfg->vad_delay_ms = vad_delay_ms;
    afe_cfg->vad_mute_playback = s_vad_mute_playback;
    afe_cfg->vad_enable_channel_trigger = false;
    afe_cfg->wakenet_init = init_wakenet;
    afe_cfg->wakenet_model_name = init_wakenet ? wn_model : NULL;
    afe_cfg->wakenet_model_name_2 = init_wakenet ? wn_model_2 : NULL;
    afe_cfg->agc_init = false;
    afe_cfg->afe_perferred_core = 1;
    afe_cfg->afe_perferred_priority = AFE_CAPTURE_AFE_TASK_PRIO;
    afe_cfg->afe_ringbuf_size = AFE_CAPTURE_AFE_RINGBUF_FRAMES;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->fixed_first_channel = AFE_CAPTURE_FIXED_FIRST_CHANNEL;
    afe_cfg->fixed_output_channel = AFE_CAPTURE_FIXED_OUTPUT_CHANNEL;
    afe_cfg->output_playback_channel = AFE_CAPTURE_OUTPUT_PLAYBACK_CHANNEL;
    if (rnnm_tuned) {
        ESP_LOGI(TAG,
                 "RNNM VAD tuned: min_speech=%dms min_noise=%dms delay=%dms",
                 vad_min_speech_ms,
                 vad_min_noise_ms,
                 vad_delay_ms);
    }
    afe_cfg->afe_linear_gain = 1.6f;
    afe_cfg = afe_config_check(afe_cfg);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "afe_config_check failed");
        afe_capture_cleanup_failed_init();
        return ESP_FAIL;
    }
    esp_err_t fd_check = validate_official_fd_afe_config(afe_cfg);
    if (fd_check != ESP_OK) {
        afe_config_free(afe_cfg);
        afe_capture_cleanup_failed_init();
        return fd_check;
    }
    log_official_fd_afe_config(afe_cfg);
    afe_config_print(afe_cfg);

    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_cfg);
        afe_capture_cleanup_failed_init();
        return ESP_FAIL;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        afe_capture_cleanup_failed_init();
        return ESP_FAIL;
    }
    log_heap("after afe create");

    s_feed_chunk_samples = s_afe_handle->get_feed_chunksize(s_afe_data);
    s_fetch_chunk_samples = s_afe_handle->get_fetch_chunksize(s_afe_data);
    s_feed_channels = s_afe_handle->get_feed_channel_num(s_afe_data);
    s_fetch_channels = s_afe_handle->get_fetch_channel_num(s_afe_data);
    s_feed_bytes = s_feed_chunk_samples * s_feed_channels * (int)sizeof(int16_t);
    if (s_feed_chunk_samples <= 0 || s_feed_channels <= 0 || s_feed_bytes <= 0) {
        ESP_LOGE(TAG, "invalid AFE feed shape samples=%d channels=%d bytes=%d",
                 s_feed_chunk_samples,
                 s_feed_channels,
                 s_feed_bytes);
        afe_capture_cleanup_failed_init();
        return ESP_FAIL;
    }

    s_initialized = true;
    apply_wakenet_runtime_state();
    if (s_afe_handle->print_pipeline) {
        s_afe_handle->print_pipeline(s_afe_data);
    }

    esp_err_t task_err = create_afe_tasks();
    if (task_err != ESP_OK) {
        afe_capture_cleanup_failed_init();
        return task_err;
    }

    log_heap("init ready");
    ESP_LOGI(TAG,
             "ready: input=%s rate=%dHz feed=%dch/%d samples fetch=%dch/%d samples afe_type=%d afe_mode=%d aec=%d aec_profile=%s aec_mode=%d aec_nlp=%d ns=%d vad=esp-sr vad_mute_playback=%d wake=%d model=%s vad_model=%s",
             s_input_format,
             s_afe_handle->get_samp_rate(s_afe_data),
             s_feed_channels,
             s_feed_chunk_samples,
             s_fetch_channels,
             s_fetch_chunk_samples,
             AFE_CAPTURE_AFE_TYPE,
             AFE_CAPTURE_AFE_MODE,
             s_has_ref_channel,
             afe_capture_get_aec_profile_name(),
             resolve_aec_mode(s_aec_profile),
             AFE_CAPTURE_AEC_NLP_LEVEL,
             s_has_ref_channel,
             s_vad_mute_playback,
             s_has_wake_model,
             wn_model ? wn_model : "none",
             vad_model ? vad_model : "webrtc");
    return ESP_OK;
}

esp_err_t afe_capture_start(afe_capture_audio_cb_t audio_cb, void *audio_ctx)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "start failed: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_OK;
    }

    s_audio_cb = audio_cb;
    s_audio_ctx = audio_ctx;
    s_capture_bytes = 0;
    s_feed_wait_log_tick = 0;
    s_fetch_log_tick = 0;
    s_capture_start_tick = xTaskGetTickCount();
    s_running = true;
    ESP_LOGI(TAG, "capture started");
    return ESP_OK;
}

esp_err_t afe_capture_start_forced(afe_capture_audio_cb_t audio_cb, void *audio_ctx)
{
    esp_err_t err = afe_capture_start(audio_cb, audio_ctx);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

uint32_t afe_capture_stop(void)
{
    if (!s_running) {
        return 0;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = (uint32_t)((now - s_capture_start_tick) * portTICK_PERIOD_MS);
    uint32_t bytes = s_capture_bytes;
    s_running = false;
    s_audio_cb = NULL;
    s_audio_ctx = NULL;
    ESP_LOGI(TAG, "capture stopped duration=%ums bytes=%u", (unsigned)duration_ms, (unsigned)bytes);
    return duration_ms;
}

bool afe_capture_is_running(void)
{
    return s_running;
}

void afe_capture_set_wake_enabled(bool enabled)
{
    s_wake_enabled = enabled;
    apply_wakenet_runtime_state();
    ESP_LOGI(TAG, "wake enabled=%d requested=%d has_model=%d", s_wake_enabled, enabled, s_has_wake_model);
}

bool afe_capture_has_wake_model(void)
{
    return s_has_wake_model;
}

afe_capture_aec_profile_t afe_capture_get_aec_profile(void)
{
    return s_aec_profile;
}

const char *afe_capture_get_aec_profile_name(void)
{
    switch (s_aec_profile) {
        case AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF:
            return "fd_high_perf";
        case AFE_CAPTURE_AEC_PROFILE_FD_LOW_COST:
        default:
            return "fd_low_cost";
    }
}

esp_err_t afe_capture_set_aec_profile(afe_capture_aec_profile_t profile)
{
    if (!aec_profile_is_valid(profile)) {
#if !ROBOT_ALLOW_EXPERIMENTAL_AEC_HIGH_PERF
        if (profile == AFE_CAPTURE_AEC_PROFILE_FD_HIGH_PERF) {
            ESP_LOGW(TAG, "AEC high performance profile disabled; define ROBOT_ALLOW_EXPERIMENTAL_AEC_HIGH_PERF=1 to test it");
            return ESP_ERR_NOT_SUPPORTED;
        }
#endif
        return ESP_ERR_INVALID_ARG;
    }
    if (s_aec_profile == profile) {
        ESP_LOGI(TAG, "AEC profile unchanged: %s", afe_capture_get_aec_profile_name());
        return ESP_OK;
    }
    if (s_running) {
        ESP_LOGW(TAG, "cannot change AEC profile while capture is running");
        return ESP_ERR_INVALID_STATE;
    }

    const char *old_name = afe_capture_get_aec_profile_name();
    s_aec_profile = profile;
    ESP_LOGI(TAG, "AEC profile change: %s -> %s", old_name, afe_capture_get_aec_profile_name());
    if (s_initialized) {
        afe_capture_deinit_afe_runtime();
    }
    return ESP_OK;
}

const char *afe_capture_get_input_format(void)
{
    return s_input_format;
}

esp_err_t afe_capture_set_input_format(const char *fmt)
{
    char normalized[AFE_CAPTURE_INPUT_FMT_MAX_LEN + 1];
    esp_err_t err = normalize_input_format(fmt, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reject input format: %s", fmt ? fmt : "(null)");
        return err;
    }
    if (strcmp(s_input_format, normalized) == 0) {
        ESP_LOGI(TAG, "input format unchanged: %s", s_input_format);
        return ESP_OK;
    }
    if (s_running) {
        ESP_LOGW(TAG, "cannot change input format while capture is running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "input format change: %s -> %s", s_input_format, normalized);
    if (s_initialized) {
        afe_capture_deinit_afe_runtime();
    }
    snprintf(s_input_format, sizeof(s_input_format), "%s", normalized);
    return ESP_OK;
}

bool afe_capture_get_vad_mute_playback(void)
{
    return s_vad_mute_playback;
}

esp_err_t afe_capture_set_vad_mute_playback(bool enabled)
{
    if (s_vad_mute_playback == enabled) {
        ESP_LOGI(TAG, "vad_mute_playback unchanged: %d", enabled);
        return ESP_OK;
    }
    if (s_running) {
        ESP_LOGW(TAG, "cannot change vad_mute_playback while capture is running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "vad_mute_playback change: %d -> %d", s_vad_mute_playback, enabled);
    s_vad_mute_playback = enabled;
    if (s_initialized) {
        afe_capture_deinit_afe_runtime();
    }
    return ESP_OK;
}

void afe_capture_set_raw_audio_callback(afe_capture_raw_audio_cb_t cb, void *ctx)
{
    s_raw_audio_ctx = ctx;
    s_raw_audio_cb = cb;
}

void afe_capture_set_raw_channel_monitor(bool enabled)
{
    s_raw_monitor_enabled = enabled;
    s_raw_monitor_log_tick = 0;
    ESP_LOGI(TAG, "raw channel monitor=%d", enabled);
}

void afe_capture_set_processed_audio_callback(afe_capture_processed_audio_cb_t cb, void *ctx)
{
    s_processed_audio_ctx = ctx;
    s_processed_audio_cb = cb;
}
