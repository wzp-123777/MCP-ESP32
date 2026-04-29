#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*mic_diag_level_cb_t)(int peak, int avg_abs);
typedef void (*mic_diag_capture_cb_t)(const uint8_t *data, int len, void *ctx);

esp_err_t mic_diag_init(mic_diag_level_cb_t level_cb);
void mic_diag_start(void);
void mic_diag_stop(void);
bool mic_diag_is_running(void);
esp_err_t mic_diag_capture_start(mic_diag_capture_cb_t capture_cb, void *ctx);
uint32_t mic_diag_capture_stop(void);
bool mic_diag_is_capturing(void);
