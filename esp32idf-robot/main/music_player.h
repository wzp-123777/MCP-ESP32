#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define MUSIC_PLAYER_LIST_LINES 4
#define MUSIC_PLAYER_TITLE_MAX 72
#define MUSIC_PLAYER_STATUS_MAX 80

typedef struct {
    bool mounted;
    bool playing;
    int track_count;
    int track_index;
    char title[MUSIC_PLAYER_TITLE_MAX];
    char status[MUSIC_PLAYER_STATUS_MAX];
    char list[MUSIC_PLAYER_LIST_LINES][MUSIC_PLAYER_TITLE_MAX];
} music_player_state_t;

typedef void (*music_player_state_cb_t)(const music_player_state_t *state, void *ctx);

esp_err_t music_player_init(void);
esp_err_t music_player_play(void);
esp_err_t music_player_stop(void);
esp_err_t music_player_next(void);
esp_err_t music_player_prev(void);
esp_err_t music_player_toggle(void);
esp_err_t music_player_refresh(void);
esp_err_t music_player_select_visible(int row);
bool music_player_is_playing(void);
const char *music_player_get_status(void);
void music_player_get_state(music_player_state_t *out);
void music_player_set_state_callback(music_player_state_cb_t cb, void *ctx);
