#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AFE_CAPTURE_EVENT_VAD_START = 0,
    AFE_CAPTURE_EVENT_VAD_END,
    AFE_CAPTURE_EVENT_WAKE,
    AFE_CAPTURE_EVENT_ERROR,
} afe_capture_event_t;

typedef void (*afe_capture_audio_cb_t)(const uint8_t *data, int len, void *ctx);
typedef void (*afe_capture_event_cb_t)(afe_capture_event_t event, void *ctx);
typedef void (*afe_capture_raw_audio_cb_t)(const int16_t *interleaved, int frames, int channels, void *ctx);
typedef void (*afe_capture_processed_audio_cb_t)(const uint8_t *data, int len, void *ctx);

esp_err_t afe_capture_init(afe_capture_event_cb_t event_cb, void *event_ctx);
esp_err_t afe_capture_start(afe_capture_audio_cb_t audio_cb, void *audio_ctx);
esp_err_t afe_capture_start_forced(afe_capture_audio_cb_t audio_cb, void *audio_ctx);
uint32_t afe_capture_stop(void);
bool afe_capture_is_running(void);
void afe_capture_set_wake_enabled(bool enabled);
bool afe_capture_has_wake_model(void);
const char *afe_capture_get_input_format(void);
esp_err_t afe_capture_set_input_format(const char *fmt);
bool afe_capture_get_vad_mute_playback(void);
esp_err_t afe_capture_set_vad_mute_playback(bool enabled);
void afe_capture_set_raw_audio_callback(afe_capture_raw_audio_cb_t cb, void *ctx);
void afe_capture_set_raw_channel_monitor(bool enabled);
void afe_capture_set_processed_audio_callback(afe_capture_processed_audio_cb_t cb, void *ctx);
