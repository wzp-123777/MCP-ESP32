#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t music_player_init(void);
esp_err_t music_player_play(void);
esp_err_t music_player_stop(void);
esp_err_t music_player_next(void);
bool music_player_is_playing(void);
const char *music_player_get_status(void);
