#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    APP_UI_STATE_IDLE = 0,
    APP_UI_STATE_RECORDING,
    APP_UI_STATE_UPLOADING,
    APP_UI_STATE_ASR,
    APP_UI_STATE_THINKING,
    APP_UI_STATE_TTS,
    APP_UI_STATE_PLAYING,
    APP_UI_STATE_ERROR,
    APP_UI_STATE_OFFLINE,
} app_ui_assistant_state_t;

typedef enum {
    APP_UI_ACTION_TALK_PRESS = 0,
    APP_UI_ACTION_TALK_RELEASE,
    APP_UI_ACTION_MCP_CONNECT,
    APP_UI_ACTION_MCP_DISCONNECT,
    APP_UI_ACTION_MCP_RECONNECT,
    APP_UI_ACTION_VOL_UP,
    APP_UI_ACTION_VOL_DOWN,
    APP_UI_ACTION_PLAY_TEST,
    APP_UI_ACTION_BLUETOOTH_TOGGLE,
} app_ui_action_t;

typedef void (*app_ui_action_cb_t)(app_ui_action_t action, void *ctx);

esp_err_t app_ui_init(void);
void app_ui_set_action_callback(app_ui_action_cb_t cb, void *ctx);
void app_ui_set_voice_state(const char *state);
void app_ui_set_mic_level(int peak, int avg_abs);
void app_ui_set_mic_state(const char *state);
void app_ui_set_mcp_status(const char *status);
void app_ui_set_volume(int volume);
void app_ui_set_bluetooth_available(bool available);
void app_ui_set_bluetooth_enabled(bool enabled);
void app_ui_set_wifi_connected(bool connected);
void app_ui_set_mcp_connected(bool connected);
void app_ui_note_mcp_activity(void);
void app_ui_set_assistant_state(app_ui_assistant_state_t state);
void app_ui_set_recent_text(const char *text);
void app_ui_next_page(void);
void app_ui_prev_page(void);
