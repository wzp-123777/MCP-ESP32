#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    int temperature_c_x10;
    int humidity_x10;
    char sensor[16];
    char detail[128];
} robot_env_reading_t;

esp_err_t robot_peripherals_init(void);
esp_err_t robot_peripherals_camera_capture(char *detail, size_t detail_size);
esp_err_t robot_peripherals_read_environment(robot_env_reading_t *reading);
esp_err_t robot_peripherals_room_light_set(bool on, char *detail, size_t detail_size);
esp_err_t robot_peripherals_room_light_toggle(char *detail, size_t detail_size);
bool robot_peripherals_room_light_is_on(void);
const char *robot_peripherals_get_status(void);
