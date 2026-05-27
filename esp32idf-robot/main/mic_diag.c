#include "mic_diag.h"

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
#ifdef CONFIG_ESP32_S3_KORVO2_V3_BOARD
#include "es7210.h"
#endif
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "i2s_stream.h"
#include "raw_stream.h"

#define MIC_SAMPLE_RATE 16000
#define MIC_BITS 16
#define MIC_READ_BYTES 1024
#define MIC_LOG_INTERVAL_MS 1000
#define MIC_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "MIC_DIAG";
static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_i2s_reader;
static audio_element_handle_t s_raw_reader;
static mic_diag_level_cb_t s_level_cb;
static mic_diag_capture_cb_t s_capture_cb;
static void *s_capture_ctx;
static volatile bool s_level_running;
static volatile bool s_capture_running;
static bool s_initialized;
static TickType_t s_capture_start_tick;
static uint32_t s_capture_bytes;

static int abs16(int16_t value)
{
    return value < 0 ? -value : value;
}

static void mic_diag_task(void *arg)
{
    int16_t *samples = calloc(1, MIC_READ_BYTES);
    if (!samples) {
        ESP_LOGE(TAG, "failed to allocate mic buffer");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int64_t last_log_tick = 0;
    while (true) {
        int bytes_read = raw_stream_read(s_raw_reader, (char *)samples, MIC_READ_BYTES);
        if (bytes_read <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool should_report = s_level_running || s_capture_running;
        if (!should_report) {
            continue;
        }

        int sample_count = bytes_read / sizeof(int16_t);
        int peak = 0;
        int64_t sum_abs = 0;
        for (int i = 0; i < sample_count; ++i) {
            int level = abs16(samples[i]);
            if (level > peak) {
                peak = level;
            }
            sum_abs += level;
        }

        int avg_abs = sample_count > 0 ? (int)(sum_abs / sample_count) : 0;
        if (s_level_cb) {
            s_level_cb(peak, avg_abs);
        }
        if (s_capture_running && s_capture_cb) {
            s_capture_cb((const uint8_t *)samples, bytes_read, s_capture_ctx);
            s_capture_bytes += (uint32_t)bytes_read;
        }

        int64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (s_level_running && now - last_log_tick >= MIC_LOG_INTERVAL_MS) {
            last_log_tick = now;
            ESP_LOGI(TAG, "level peak=%d avg=%d", peak, avg_abs);
        }
    }
}

esp_err_t mic_diag_init(mic_diag_level_cb_t level_cb)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_level_cb = level_cb;

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
        audio_hal_ctrl_codec(board->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    }
#ifdef CONFIG_ESP32_S3_KORVO2_V3_BOARD
    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_30DB);
    es7210_adc_set_gain(ES7210_INPUT_MIC2 | ES7210_INPUT_MIC1, GAIN_33DB);
#endif

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipeline) {
        ESP_LOGE(TAG, "audio_pipeline_init failed");
        return ESP_FAIL;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_TYLE_AND_CH(CODEC_ADC_I2S_PORT,
                                                                        MIC_SAMPLE_RATE,
                                                                        I2S_DATA_BIT_WIDTH_16BIT,
                                                                        AUDIO_STREAM_READER,
                                                                        I2S_SLOT_MODE_MONO);
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.task_stack = 4096;
    i2s_cfg.stack_in_ext = true;
    i2s_cfg.out_rb_size = 8 * 1024;
    s_i2s_reader = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_reader) {
        ESP_LOGE(TAG, "i2s_stream_init failed");
        return ESP_FAIL;
    }

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 8 * 1024;
    s_raw_reader = raw_stream_init(&raw_cfg);
    if (!s_raw_reader) {
        ESP_LOGE(TAG, "raw_stream_init failed");
        return ESP_FAIL;
    }

    audio_pipeline_register(s_pipeline, s_i2s_reader, "i2s");
    audio_pipeline_register(s_pipeline, s_raw_reader, "raw");
    const char *link_tag[2] = {"i2s", "raw"};
    if (audio_pipeline_link(s_pipeline, link_tag, 2) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_link failed");
        return ESP_FAIL;
    }

    if (i2s_stream_set_clk(s_i2s_reader, MIC_SAMPLE_RATE, MIC_BITS, 1) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_stream_set_clk failed");
        return ESP_FAIL;
    }

    if (audio_pipeline_run(s_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        return ESP_FAIL;
    }

    if (xTaskCreateWithCaps(mic_diag_task, "mic_diag", 4096, NULL, 4, NULL, MIC_TASK_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "mic task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "ready; type MIC ON to print levels");
    return ESP_OK;
}

void mic_diag_start(void)
{
    s_level_running = true;
    ESP_LOGI(TAG, "enabled");
}

void mic_diag_stop(void)
{
    s_level_running = false;
    ESP_LOGI(TAG, "disabled");
}

bool mic_diag_is_running(void)
{
    return s_level_running;
}

esp_err_t mic_diag_capture_start(mic_diag_capture_cb_t capture_cb, void *ctx)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "capture start failed: mic not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_capture_running) {
        return ESP_OK;
    }

    s_capture_cb = capture_cb;
    s_capture_ctx = ctx;
    s_capture_bytes = 0;
    s_capture_start_tick = xTaskGetTickCount();
    s_capture_running = true;
    ESP_LOGI(TAG, "capture started");
    return ESP_OK;
}

uint32_t mic_diag_capture_stop(void)
{
    if (!s_capture_running) {
        return 0;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t duration_ms = (uint32_t)((now - s_capture_start_tick) * portTICK_PERIOD_MS);
    s_capture_running = false;
    s_capture_cb = NULL;
    s_capture_ctx = NULL;
    ESP_LOGI(TAG, "capture stopped duration=%ums bytes=%u", (unsigned)duration_ms, (unsigned)s_capture_bytes);
    return duration_ms;
}

bool mic_diag_is_capturing(void)
{
    return s_capture_running;
}
