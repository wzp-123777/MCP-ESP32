#include "audio_player.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_element.h"
#include "audio_hal.h"
#include "audio_pipeline.h"
#include "board.h"
#include "esp_log.h"
#include "i2s_stream.h"
#include "raw_stream.h"

extern const uint8_t xiaole_pcm_start[] asm("_binary_xiaole_16k_stereo_pcm_start");
extern const uint8_t xiaole_pcm_end[] asm("_binary_xiaole_16k_stereo_pcm_end");

#define PLAYER_SAMPLE_RATE 16000
#define PLAYER_BITS 16
#define PLAYER_CHANNELS 1
#define PLAYER_CHUNK_BYTES 2048
#define PLAYER_INITIAL_VOLUME 45
#define PLAYER_SOFT_LIMIT 26000

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
} wav_format_t;

static const char *TAG = "AUDIO_PLAYER";
static audio_board_handle_t s_board;
static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_raw_writer;
static audio_element_handle_t s_i2s_writer;
static int s_volume = PLAYER_INITIAL_VOLUME;

static int clamp_volume(int volume)
{
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static bool find_wav_data(const uint8_t *wav, size_t wav_len, wav_format_t *fmt, const uint8_t **pcm, size_t *pcm_len)
{
    if (!wav || wav_len < 44 || !fmt || !pcm || !pcm_len) {
        return false;
    }
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    size_t offset = 12;
    while (offset + 8 <= wav_len) {
        const uint8_t *chunk = wav + offset;
        uint32_t chunk_size = read_u32_le(chunk + 4);
        size_t payload = offset + 8;
        if (payload + chunk_size > wav_len) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }
            fmt->audio_format = read_u16_le(wav + payload);
            fmt->channels = read_u16_le(wav + payload + 2);
            fmt->sample_rate = read_u32_le(wav + payload + 4);
            fmt->bits_per_sample = read_u16_le(wav + payload + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt) {
                return false;
            }
            *pcm = wav + payload;
            *pcm_len = chunk_size;
            return true;
        }

        offset = payload + chunk_size + (chunk_size & 1U);
    }
    return false;
}

static void log_wav_head(const uint8_t *wav, size_t wav_len, const char *tag)
{
    char head[96] = {0};
    size_t count = wav_len < 32 ? wav_len : 32;
    size_t pos = 0;
    for (size_t i = 0; i < count && pos + 3 < sizeof(head); ++i) {
        pos += snprintf(head + pos, sizeof(head) - pos, "%02X ", wav[i]);
    }
    ESP_LOGE(TAG, "%s wav head len=%u: %s", tag ? tag : "wav", (unsigned)wav_len, head);
}

static int16_t clamp16(int value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (int16_t)value;
}

static int sample_at_mono16(const int16_t *samples, size_t frames, uint16_t channels, size_t frame_index)
{
    if (frame_index >= frames) {
        frame_index = frames ? frames - 1 : 0;
    }
    if (channels == 1) {
        return samples[frame_index];
    }
    int left = samples[frame_index * channels];
    int right = samples[frame_index * channels + 1];
    return (left + right) / 2;
}

static int16_t soft_limit16(int value)
{
    if (value > PLAYER_SOFT_LIMIT) {
        value = PLAYER_SOFT_LIMIT + ((value - PLAYER_SOFT_LIMIT) / 4);
    } else if (value < -PLAYER_SOFT_LIMIT) {
        value = -PLAYER_SOFT_LIMIT + ((value + PLAYER_SOFT_LIMIT) / 4);
    }
    return clamp16(value);
}

static int16_t *wav_to_16k_mono(const wav_format_t *fmt, const uint8_t *pcm, size_t pcm_len, size_t *out_bytes)
{
    if (!fmt || !pcm || !out_bytes || fmt->audio_format != 1 || fmt->bits_per_sample != 16 ||
        (fmt->channels != 1 && fmt->channels != 2) || fmt->sample_rate == 0) {
        return NULL;
    }

    size_t source_frames = pcm_len / (sizeof(int16_t) * fmt->channels);
    if (source_frames == 0) {
        return NULL;
    }

    size_t target_frames = ((uint64_t)source_frames * PLAYER_SAMPLE_RATE) / fmt->sample_rate;
    if (target_frames == 0) {
        target_frames = 1;
    }
    size_t target_samples = target_frames * PLAYER_CHANNELS;
    if (target_samples > (SIZE_MAX / sizeof(int16_t))) {
        return NULL;
    }

    int16_t *mono16 = calloc(target_samples, sizeof(int16_t));
    if (!mono16) {
        return NULL;
    }

    const int16_t *samples = (const int16_t *)pcm;
    for (size_t frame = 0; frame < target_frames; ++frame) {
        uint64_t pos_q16 = ((uint64_t)frame * fmt->sample_rate << 16) / PLAYER_SAMPLE_RATE;
        size_t index = (size_t)(pos_q16 >> 16);
        uint32_t frac = (uint32_t)(pos_q16 & 0xFFFF);
        int a = sample_at_mono16(samples, source_frames, fmt->channels, index);
        int b = sample_at_mono16(samples, source_frames, fmt->channels, index + 1);
        int mono = a + (((b - a) * (int)frac) >> 16);
        mono16[frame] = soft_limit16(mono);
    }

    *out_bytes = target_samples * sizeof(int16_t);
    return mono16;
}

static esp_err_t audio_player_write_pcm(const uint8_t *pcm, size_t len, const char *tag)
{
    const uint8_t *cursor = pcm;
    const uint8_t *end = pcm + len;
    size_t bytes_written = 0;

    while (cursor < end) {
        int chunk = (int)(end - cursor);
        if (chunk > PLAYER_CHUNK_BYTES) {
            chunk = PLAYER_CHUNK_BYTES;
        }

        int written = raw_stream_write(s_raw_writer, (char *)cursor, chunk);
        if (written < 0) {
            ESP_LOGE(TAG, "%s raw_stream_write failed ret=%d", tag ? tag : "pcm", written);
            return ESP_FAIL;
        }
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        cursor += written;
        bytes_written += written;
    }

    ESP_LOGI(TAG, "%s pcm written bytes=%u", tag ? tag : "pcm", (unsigned)bytes_written);
    return ESP_OK;
}

void audio_player_set_volume(int volume)
{
    s_volume = clamp_volume(volume);
    if (s_board && s_board->audio_hal) {
        audio_hal_set_volume(s_board->audio_hal, s_volume);
        audio_hal_set_mute(s_board->audio_hal, s_volume == 0);
    }
    ESP_LOGI(TAG, "volume=%d", s_volume);
}

void audio_player_adjust_volume(int delta)
{
    audio_player_set_volume(s_volume + delta);
}

int audio_player_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_player_init(void)
{
    ESP_LOGI(TAG, "init board codec");
    s_board = audio_board_init();
    if (!s_board || !s_board->audio_hal) {
        ESP_LOGE(TAG, "audio_board_init failed");
        return ESP_FAIL;
    }

    audio_hal_ctrl_codec(s_board->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    audio_hal_enable_pa(s_board->audio_hal, true);
    audio_hal_set_mute(s_board->audio_hal, false);
    audio_player_set_volume(PLAYER_INITIAL_VOLUME);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!s_pipeline) {
        ESP_LOGE(TAG, "audio_pipeline_init failed");
        return ESP_FAIL;
    }

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 16 * 1024;
    s_raw_writer = raw_stream_init(&raw_cfg);
    if (!s_raw_writer) {
        ESP_LOGE(TAG, "raw_stream_init failed");
        return ESP_FAIL;
    }

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, PLAYER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.task_stack = 4096;
    i2s_cfg.out_rb_size = 16 * 1024;
    s_i2s_writer = i2s_stream_init(&i2s_cfg);
    if (!s_i2s_writer) {
        ESP_LOGE(TAG, "i2s_stream_init failed");
        return ESP_FAIL;
    }

    audio_pipeline_register(s_pipeline, s_raw_writer, "raw");
    audio_pipeline_register(s_pipeline, s_i2s_writer, "i2s");
    const char *link_tag[2] = {"raw", "i2s"};
    if (audio_pipeline_link(s_pipeline, link_tag, 2) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_link failed");
        return ESP_FAIL;
    }

    if (i2s_stream_set_clk(s_i2s_writer, PLAYER_SAMPLE_RATE, PLAYER_BITS, PLAYER_CHANNELS) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_stream_set_clk failed");
        return ESP_FAIL;
    }

    if (audio_pipeline_run(s_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "audio_pipeline_run failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ready: raw -> i2s -> es8311, %dHz %dbit %dch", PLAYER_SAMPLE_RATE, PLAYER_BITS, PLAYER_CHANNELS);
    return ESP_OK;
}

esp_err_t audio_player_play_xiaole(void)
{
    const uint8_t *pcm = xiaole_pcm_start;
    size_t bytes_total = xiaole_pcm_end - xiaole_pcm_start;

    ESP_LOGI(TAG, "play xiaole pcm bytes=%u frames=%u volume=%d", (unsigned)bytes_total, (unsigned)(bytes_total / 4), s_volume);

    esp_err_t ret = audio_player_write_pcm(pcm, bytes_total, "xiaole");
    vTaskDelay(pdMS_TO_TICKS(180));
    ESP_LOGI(TAG, "play done");
    return ret;
}

esp_err_t audio_player_play_wav(const uint8_t *wav, size_t wav_len, const char *tag)
{
    wav_format_t fmt = {0};
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    size_t out_bytes = 0;

    if (!find_wav_data(wav, wav_len, &fmt, &pcm, &pcm_len)) {
        ESP_LOGE(TAG, "%s wav parse failed len=%u", tag ? tag : "wav", (unsigned)wav_len);
        log_wav_head(wav, wav_len, tag);
        return ESP_FAIL;
    }

    int16_t *mono16 = wav_to_16k_mono(&fmt, pcm, pcm_len, &out_bytes);
    if (!mono16) {
        ESP_LOGE(TAG, "%s wav unsupported format=%u rate=%u bits=%u ch=%u",
                 tag ? tag : "wav",
                 fmt.audio_format,
                 (unsigned)fmt.sample_rate,
                 fmt.bits_per_sample,
                 fmt.channels);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "play %s wav=%u rate=%u ch=%u pcm=%u -> 16k mono=%u",
             tag ? tag : "wav",
             (unsigned)wav_len,
             (unsigned)fmt.sample_rate,
             fmt.channels,
             (unsigned)pcm_len,
             (unsigned)out_bytes);

    esp_err_t ret = audio_player_write_pcm((const uint8_t *)mono16, out_bytes, tag ? tag : "wav");
    free(mono16);
    vTaskDelay(pdMS_TO_TICKS(180));
    return ret;
}
