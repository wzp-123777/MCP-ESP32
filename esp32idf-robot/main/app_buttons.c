#include "app_buttons.h"

#include "board.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "input_key_service.h"

static const char *TAG = "APP_BUTTONS";
static esp_periph_set_handle_t s_periph_set;
static periph_service_handle_t s_input_service;
static app_button_cb_t s_cb;
static void *s_cb_ctx;
static bool s_set_active;

static void emit_button(app_button_event_t event)
{
    if (s_cb) {
        s_cb(event, s_cb_ctx);
    }
}

static bool is_press_event(int type)
{
    return type == INPUT_KEY_SERVICE_ACTION_CLICK || type == INPUT_KEY_SERVICE_ACTION_PRESS;
}

static bool is_release_event(int type)
{
    return type == INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE || type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE;
}

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    (void)handle;
    (void)ctx;

    const int key_id = (int)evt->data;
    const int type = evt->type;

    switch (key_id) {
        case INPUT_KEY_USER_ID_SET:
            if (is_press_event(type) && !s_set_active) {
                s_set_active = true;
                ESP_LOGI(TAG, "SET press");
                emit_button(APP_BUTTON_SET_PRESS);
            } else if (is_release_event(type) && s_set_active) {
                s_set_active = false;
                ESP_LOGI(TAG, "SET release");
                emit_button(APP_BUTTON_SET_RELEASE);
            }
            break;
        case INPUT_KEY_USER_ID_VOLUP:
            if (is_release_event(type) || type == INPUT_KEY_SERVICE_ACTION_CLICK) {
                emit_button(APP_BUTTON_VOL_UP);
            }
            break;
        case INPUT_KEY_USER_ID_VOLDOWN:
            if (is_release_event(type) || type == INPUT_KEY_SERVICE_ACTION_CLICK) {
                emit_button(APP_BUTTON_VOL_DOWN);
            }
            break;
        case INPUT_KEY_USER_ID_PLAY:
            if (is_release_event(type)) {
                emit_button(APP_BUTTON_PLAY);
            }
            break;
        case INPUT_KEY_USER_ID_MUTE:
        case INPUT_KEY_USER_ID_MODE:
            if (is_release_event(type)) {
                emit_button(APP_BUTTON_MODE);
            }
            break;
        case INPUT_KEY_USER_ID_REC:
            if (is_release_event(type)) {
                emit_button(APP_BUTTON_REC);
            }
            break;
        default:
            ESP_LOGD(TAG, "ignored key id=%d type=%d", key_id, type);
            break;
    }

    return ESP_OK;
}

esp_err_t app_buttons_init(app_button_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (!s_periph_set) {
        ESP_LOGE(TAG, "esp_periph_set_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = audio_board_key_init(s_periph_set);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_board_key_init failed: %s", esp_err_to_name(err));
        return err;
    }

    input_key_service_info_t input_key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t input_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    input_cfg.handle = s_periph_set;
    input_cfg.based_cfg.task_stack = 4 * 1024;

    s_input_service = input_key_service_create(&input_cfg);
    if (!s_input_service) {
        ESP_LOGE(TAG, "input_key_service_create failed");
        return ESP_FAIL;
    }

    input_key_service_add_key(s_input_service, input_key_info, INPUT_KEY_NUM);
    periph_service_set_callback(s_input_service, input_key_service_cb, NULL);

    ESP_LOGI(TAG, "ready: hold SET to record, VOL buttons adjust volume");
    return ESP_OK;
}
