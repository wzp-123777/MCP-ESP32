#pragma once

#include "esp_err.h"

esp_err_t app_ui_init(void);
void app_ui_set_voice_state(const char *state);
void app_ui_set_mic_level(int peak, int avg_abs);
void app_ui_set_mic_state(const char *state);
void app_ui_set_mcp_status(const char *status);
void app_ui_set_volume(int volume);
