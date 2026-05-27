#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_element.h"
#include "esp_err.h"

typedef void (*audio_player_reference_tap_cb_t)(const int16_t *samples, int frames, void *ctx);

esp_err_t audio_player_init(void);
esp_err_t audio_player_play_xiaole(void);
esp_err_t audio_player_play_barge_after_prompt(void);
esp_err_t audio_player_play_barge_interrupt_prompt(void);
esp_err_t audio_player_play_wav(const uint8_t *wav, size_t wav_len, const char *tag);
esp_err_t audio_player_play_wav_ex(const uint8_t *wav, size_t wav_len, const char *tag, bool preroll, uint32_t tail_delay_ms);
esp_err_t audio_player_play_pcm16(const uint8_t *pcm, size_t len, const char *tag, bool preroll);
esp_err_t audio_player_stream_begin(const char *tag, bool preroll);
esp_err_t audio_player_stream_write_pcm16(const uint8_t *pcm, size_t len, const char *tag);
void audio_player_stream_end(const char *tag, uint32_t tail_delay_ms);
bool audio_player_cancel_requested(void);
esp_err_t audio_player_write_silence_ms(uint32_t duration_ms, const char *tag);
void audio_player_cancel(void);
void audio_player_set_volume(int volume);
void audio_player_adjust_volume(int delta);
int audio_player_get_volume(void);
void audio_player_set_output_target(audio_element_handle_t target);
void audio_player_set_reference_tap_callback(audio_player_reference_tap_cb_t cb, void *ctx);
