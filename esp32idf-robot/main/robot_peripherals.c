#include "robot_peripherals.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "tca9554.h"

#ifndef ROBOT_ROOM_LIGHT_TCA_GPIO
#define ROBOT_ROOM_LIGHT_TCA_GPIO BLUE_LED_GPIO
#endif

#ifndef ROBOT_ROOM_LIGHT_GPIO
#define ROBOT_ROOM_LIGHT_GPIO -1
#endif

#ifndef ROBOT_ROOM_LIGHT_ACTIVE_LOW
#define ROBOT_ROOM_LIGHT_ACTIVE_LOW 0
#endif

#ifndef ROBOT_CAMERA_REAL_CAPTURE
#define ROBOT_CAMERA_REAL_CAPTURE 0
#endif

#define ENV_I2C_TIMEOUT_MS 120
#define ENV_I2C_CLK_HZ 100000
#define AHT20_ADDR 0x38
#define SHT30_ADDR_PRIMARY 0x44
#define SHT30_ADDR_SECONDARY 0x45

static const char *TAG = "ROBOT_PERIPH";
static SemaphoreHandle_t s_lock;
static bool s_room_light_on;
static bool s_tca_light_ready;
static char s_status[96] = "PERIPH READY";

static void set_status(const char *status)
{
    if (!status) {
        return;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    strlcpy(s_status, status, sizeof(s_status));
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "%s", status);
}

const char *robot_peripherals_get_status(void)
{
    return s_status;
}

bool robot_peripherals_room_light_is_on(void)
{
    bool on = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    on = s_room_light_on;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return on;
}

static void format_detail(char *detail, size_t detail_size, const char *fallback)
{
    if (detail && detail_size > 0) {
        strlcpy(detail, fallback ? fallback : "", detail_size);
    }
}

static esp_err_t add_i2c_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_master_handle(I2C_NUM_0);
    if (!bus || !dev) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ENV_I2C_CLK_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, dev);
}

static esp_err_t read_aht20(robot_env_reading_t *reading)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = add_i2c_device(AHT20_ADDR, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
    ret = i2c_master_transmit(dev, init_cmd, sizeof(init_cmd), pdMS_TO_TICKS(ENV_I2C_TIMEOUT_MS));
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t trigger_cmd[3] = {0xAC, 0x33, 0x00};
        ret = i2c_master_transmit(dev, trigger_cmd, sizeof(trigger_cmd), pdMS_TO_TICKS(ENV_I2C_TIMEOUT_MS));
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(80));
        uint8_t data[7] = {0};
        ret = i2c_master_receive(dev, data, sizeof(data), pdMS_TO_TICKS(ENV_I2C_TIMEOUT_MS));
        if (ret == ESP_OK && (data[0] & 0x80) == 0) {
            uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
            uint32_t raw_t = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
            reading->humidity_x10 = (int)(((uint64_t)raw_h * 1000) >> 20);
            reading->temperature_c_x10 = (int)(((uint64_t)raw_t * 2000) >> 20) - 500;
            strlcpy(reading->sensor, "AHT20", sizeof(reading->sensor));
            reading->valid = true;
            snprintf(reading->detail,
                     sizeof(reading->detail),
                     "AHT20 temp=%d.%dC humidity=%d.%d%%",
                     reading->temperature_c_x10 / 10,
                     abs(reading->temperature_c_x10 % 10),
                     reading->humidity_x10 / 10,
                     abs(reading->humidity_x10 % 10));
        } else if (ret == ESP_OK) {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }

    i2c_master_bus_rm_device(dev);
    return ret;
}

static esp_err_t read_sht30_addr(uint8_t addr, robot_env_reading_t *reading)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = add_i2c_device(addr, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t cmd[2] = {0x2C, 0x06};
    ret = i2c_master_transmit(dev, cmd, sizeof(cmd), pdMS_TO_TICKS(ENV_I2C_TIMEOUT_MS));
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t data[6] = {0};
        ret = i2c_master_receive(dev, data, sizeof(data), pdMS_TO_TICKS(ENV_I2C_TIMEOUT_MS));
        if (ret == ESP_OK) {
            uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
            uint16_t raw_h = ((uint16_t)data[3] << 8) | data[4];
            reading->temperature_c_x10 = -450 + (int)(((uint32_t)raw_t * 1750) / 65535);
            reading->humidity_x10 = (int)(((uint32_t)raw_h * 1000) / 65535);
            strlcpy(reading->sensor, addr == SHT30_ADDR_PRIMARY ? "SHT30" : "SHT30-45", sizeof(reading->sensor));
            reading->valid = true;
            snprintf(reading->detail,
                     sizeof(reading->detail),
                     "%s temp=%d.%dC humidity=%d.%d%%",
                     reading->sensor,
                     reading->temperature_c_x10 / 10,
                     abs(reading->temperature_c_x10 % 10),
                     reading->humidity_x10 / 10,
                     abs(reading->humidity_x10 % 10));
        }
    }

    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t robot_peripherals_read_environment(robot_env_reading_t *reading)
{
    if (!reading) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(reading, 0, sizeof(*reading));
    if (!i2c_bus_get_master_handle(I2C_NUM_0)) {
        strlcpy(reading->detail, "I2C bus not ready; sensor should use Korvo-2 GPIO17/GPIO18", sizeof(reading->detail));
        set_status("ENV I2C OFF");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = read_aht20(reading);
    if (ret != ESP_OK) {
        ret = read_sht30_addr(SHT30_ADDR_PRIMARY, reading);
    }
    if (ret != ESP_OK) {
        ret = read_sht30_addr(SHT30_ADDR_SECONDARY, reading);
    }
    if (ret == ESP_OK && reading->valid) {
        set_status("ENV OK");
        return ESP_OK;
    }
    snprintf(reading->detail,
             sizeof(reading->detail),
             "no AHT20/SHT30 humidity sensor on I2C GPIO17/GPIO18 ret=%s",
             esp_err_to_name(ret));
    set_status("ENV MISS");
    return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
}

static esp_err_t set_external_room_light(bool on)
{
#if ROBOT_ROOM_LIGHT_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ROBOT_ROOM_LIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "room light gpio config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    int level = on ? 1 : 0;
#if ROBOT_ROOM_LIGHT_ACTIVE_LOW
    level = !level;
#endif
    return gpio_set_level((gpio_num_t)ROBOT_ROOM_LIGHT_GPIO, level);
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t set_board_room_light(bool on)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_master_handle(I2C_NUM_0);
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = tca9554_set_io_config((esp_tca9554_gpio_num_t)ROBOT_ROOM_LIGHT_TCA_GPIO, TCA9554_IO_OUTPUT);
    if (ret != ESP_OK) {
        return ret;
    }
    s_tca_light_ready = true;
    esp_tca9554_io_level_t level = on ? TCA9554_IO_HIGH : TCA9554_IO_LOW;
#if ROBOT_ROOM_LIGHT_ACTIVE_LOW
    level = on ? TCA9554_IO_LOW : TCA9554_IO_HIGH;
#endif
    return tca9554_set_output_state((esp_tca9554_gpio_num_t)ROBOT_ROOM_LIGHT_TCA_GPIO, level);
}

esp_err_t robot_peripherals_room_light_set(bool on, char *detail, size_t detail_size)
{
    esp_err_t ret = set_external_room_light(on);
    const char *target = "external-gpio";
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        target = "korvo2-board-led";
        ret = set_board_room_light(on);
    }
    if (ret == ESP_OK) {
        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        s_room_light_on = on;
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }
        char status[32];
        snprintf(status, sizeof(status), "LIGHT %s", on ? "ON" : "OFF");
        set_status(status);
        if (detail && detail_size > 0) {
            snprintf(detail, detail_size, "room_light=%s target=%s", on ? "on" : "off", target);
        }
    } else {
        set_status("LIGHT FAIL");
        if (detail && detail_size > 0) {
            snprintf(detail,
                     detail_size,
                     "room_light failed ret=%s target=%s; external GPIO disabled and board LED may be unavailable",
                     esp_err_to_name(ret),
                     target);
        }
    }
    return ret;
}

esp_err_t robot_peripherals_room_light_toggle(char *detail, size_t detail_size)
{
    return robot_peripherals_room_light_set(!robot_peripherals_room_light_is_on(), detail, detail_size);
}

esp_err_t robot_peripherals_camera_capture(char *detail, size_t detail_size)
{
#if ROBOT_CAMERA_REAL_CAPTURE
    format_detail(detail, detail_size, "camera real capture is enabled but driver backend is not linked");
    set_status("CAMERA TODO");
    return ESP_ERR_NOT_SUPPORTED;
#else
    format_detail(
        detail,
        detail_size,
        "camera scaffold ready; OV camera connector pins xclk=40 siod=17 sioc=18 d0=13 d1=47 d2=14 d3=3 d4=12 d5=42 d6=41 d7=39 vsync=21 href=38 pclk=11");
    set_status("CAMERA WAIT");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t robot_peripherals_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    char detail[96];
    esp_err_t ret = robot_peripherals_room_light_set(false, detail, sizeof(detail));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "room light fallback init failed: %s (%s)", esp_err_to_name(ret), detail);
    }
    ESP_LOGI(TAG,
             "camera pins prepared xclk=%d siod=%d sioc=%d d0=%d d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d vsync=%d href=%d pclk=%d",
             CAM_PIN_XCLK,
             CAM_PIN_SIOD,
             CAM_PIN_SIOC,
             CAM_PIN_D0,
             CAM_PIN_D1,
             CAM_PIN_D2,
             CAM_PIN_D3,
             CAM_PIN_D4,
             CAM_PIN_D5,
             CAM_PIN_D6,
             CAM_PIN_D7,
             CAM_PIN_VSYNC,
             CAM_PIN_HREF,
             CAM_PIN_PCLK);
    set_status(s_tca_light_ready ? "PERIPH READY" : "PERIPH PARTIAL");
    return ESP_OK;
}
