#pragma once

#include "esp_err.h"

typedef enum {
    APP_BUTTON_SET_PRESS,
    APP_BUTTON_SET_RELEASE,
    APP_BUTTON_VOL_UP,
    APP_BUTTON_VOL_DOWN,
    APP_BUTTON_PLAY,
    APP_BUTTON_MODE,
    APP_BUTTON_REC,
} app_button_event_t;

typedef void (*app_button_cb_t)(app_button_event_t event, void *ctx);

esp_err_t app_buttons_init(app_button_cb_t cb, void *ctx);
