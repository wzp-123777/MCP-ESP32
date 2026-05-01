#include "app_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lvgl.h"

#define UI_W LCD_H_RES
#define UI_H LCD_V_RES
#define UI_DRAW_LINES 40
#define UI_LEVEL_REFRESH_MS 200
#define UI_LOOP_INTERVAL_MS 20
#define UI_RENDER_INTERVAL_MS 150
#define UI_CLOCK_REFRESH_MS 1000
#define UI_MCP_OFFLINE_TIMEOUT_MS 10000
#define UI_LVGL_TICK_MS 5
#define UI_PAGE_COUNT 5
#define UI_ACTION_QUEUE_LEN 8
#define UI_TOUCH_I2C_CLK 100000
#define UI_TOUCH_TT21100_ADDR 0x24
#define UI_TOUCH_GT911_ADDR 0x5D
#define UI_TOUCH_GT911_ADDR_BACKUP 0x14
#define UI_TOUCH_I2C_TIMEOUT_MS 8
#define UI_TOUCH_REPORT_HEADER_LEN 7
#define UI_TOUCH_RECORD_LEN 10
#define UI_TOUCH_REPORT_MAX_LEN 64
#define UI_TOUCH_GT911_REG_PRODUCT_ID 0x8140
#define UI_TOUCH_GT911_REG_CONFIG_VERSION 0x8047
#define UI_TOUCH_GT911_REG_STATUS 0x814E
#define UI_TOUCH_GT911_REG_FIRST_POINT 0x814F
#define UI_TOUCH_GT911_POINT_RECORD_LEN 8
#define UI_TOUCH_GT911_MAX_POINTS 5
#define UI_TOUCH_RELEASE_GRACE_MS 70
#define UI_TOUCH_SWAP_XY 0
#define UI_TOUCH_MIRROR_X 0
#define UI_TOUCH_MIRROR_Y 0

#define UI_BG_COLOR 0xFFF5FA
#define UI_PANEL_COLOR 0xFFFCFE
#define UI_SOFT_PANEL_COLOR 0xFFF0F7
#define UI_BORDER_COLOR 0xF0CFE0
#define UI_TEXT_COLOR 0x51394A
#define UI_MUTED_COLOR 0x9A7F90
#define UI_ACCENT_COLOR 0xEF8ABC
#define UI_ACCENT_SOFT_COLOR 0xFFD9EA
#define UI_OK_COLOR 0x35B86D
#define UI_BLUE_COLOR 0x57A6D8
#define UI_ERROR_COLOR 0xE34D6F

#if LV_FONT_SIMSUN_16_CJK
LV_FONT_DECLARE(lv_font_simsun_16_cjk);
#define UI_FONT_CJK (&lv_font_simsun_16_cjk)
#define UI_FONT_RECENT (&lv_font_simsun_16_cjk)
#define UI_FONT_TEXT (&lv_font_simsun_16_cjk)
#else
#define UI_FONT_CJK (&lv_font_montserrat_14)
#define UI_FONT_RECENT (&lv_font_montserrat_14)
#define UI_FONT_TEXT (&lv_font_montserrat_14)
#endif

static const char *TAG = "APP_UI";
static esp_periph_set_handle_t s_periph_set;
static esp_lcd_panel_handle_t s_panel;
static lv_disp_drv_t s_disp_drv;
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf1;
static lv_color_t *s_buf2;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick_timer;
static i2c_master_dev_handle_t s_touch_dev;
static lv_indev_drv_t s_touch_drv;
static uint8_t s_touch_addr;
static bool s_touch_is_gt911;
static bool s_touch_ready;
static bool s_touch_pressed;
static int16_t s_touch_x = UI_W / 2;
static int16_t s_touch_y = UI_H / 2;
static TickType_t s_last_touch_tick;
static uint16_t s_touch_fail_count;
static bool s_ready;
static bool s_dirty;
static bool s_page_dirty;
static bool s_clock_dirty;
static bool s_wifi_connected;
static bool s_mcp_connected;
static bool s_bt_available;
static bool s_bt_enabled;
static app_ui_assistant_state_t s_assistant_state = APP_UI_STATE_IDLE;
static char s_voice_state[32] = "VOICE READY";
static char s_mic_state[32] = "MIC OFF";
static char s_mcp_status[48] = "MCP NOT CONFIGURED";
static char s_recent_text[128] = "";
static int s_volume = 45;
static int s_mic_peak;
static int s_mic_avg;
static uint8_t s_page;
static TickType_t s_last_level_render_tick;
static TickType_t s_last_mcp_activity_tick;
static TickType_t s_last_render_tick;
static TickType_t s_last_clock_tick;
static app_ui_action_cb_t s_action_cb;
static void *s_action_ctx;
static QueueHandle_t s_action_queue;

static lv_obj_t *s_pages[UI_PAGE_COUNT];
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_mcp_label;
static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_mcp_dot;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_page_label;
static lv_obj_t *s_page_dots[UI_PAGE_COUNT];
static lv_obj_t *s_home_time_label;
static lv_obj_t *s_home_status_label;

static lv_obj_t *s_state_label;
static lv_obj_t *s_state_caption_label;
static lv_obj_t *s_status_track;
static lv_obj_t *s_status_line;
static lv_obj_t *s_recent_label;
static lv_obj_t *s_reply_label;
static lv_obj_t *s_talk_button;
static lv_obj_t *s_talk_button_label;

static lv_obj_t *s_chat_in_label;
static lv_obj_t *s_chat_out_label;
static lv_obj_t *s_chat_system_label;

static lv_obj_t *s_net_wifi_label;
static lv_obj_t *s_net_mcp_label;
static lv_obj_t *s_net_endpoint_label;
static lv_obj_t *s_set_bt_label;
static lv_obj_t *s_set_time_label;
static lv_obj_t *s_set_status_label;

static lv_obj_t *s_dbg_network_label;
static lv_obj_t *s_dbg_audio_label;
static lv_obj_t *s_dbg_phase_label;
static lv_obj_t *s_dbg_mic_label;
static lv_obj_t *s_dbg_error_label;
static lv_obj_t *s_mic_bar;

static void emit_action(app_ui_action_t action);
static void action_dispatch_task(void *arg);
static void set_page_locked(uint8_t page);

static const char *state_text(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_RECORDING:
            return "录音中";
        case APP_UI_STATE_UPLOADING:
            return "上传中";
        case APP_UI_STATE_ASR:
            return "识别中";
        case APP_UI_STATE_THINKING:
            return "思考中";
        case APP_UI_STATE_TTS:
            return "合成中";
        case APP_UI_STATE_PLAYING:
            return "播放中";
        case APP_UI_STATE_ERROR:
            return "错误";
        case APP_UI_STATE_OFFLINE:
            return "离线";
        case APP_UI_STATE_IDLE:
        default:
            return "待机";
    }
}

static const char *state_caption(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_RECORDING:
            return "按住 SET，松开发送";
        case APP_UI_STATE_UPLOADING:
            return "正在上传音频";
        case APP_UI_STATE_ASR:
            return "正在语音识别";
        case APP_UI_STATE_THINKING:
            return "大模型思考中";
        case APP_UI_STATE_TTS:
            return "正在合成语音";
        case APP_UI_STATE_PLAYING:
            return "播放 16k 单声道";
        case APP_UI_STATE_ERROR:
            return "出错，查看调试页";
        case APP_UI_STATE_OFFLINE:
            return "网络或 MCP 离线";
        case APP_UI_STATE_IDLE:
        default:
            return "待机，按住说话";
    }
}

static const char *page_title(uint8_t page)
{
    switch (page) {
        case 1:
            return "对话";
        case 2:
            return "网络";
        case 3:
            return "设置";
        case 4:
            return "调试";
        case 0:
        default:
            return "首页";
    }
}

static const char *home_state_hint(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_RECORDING:
            return "正在录音";
        case APP_UI_STATE_UPLOADING:
            return "发送音频到 MCP";
        case APP_UI_STATE_ASR:
            return "正在识别语音";
        case APP_UI_STATE_THINKING:
            return "等待模型回复";
        case APP_UI_STATE_TTS:
            return "正在生成语音";
        case APP_UI_STATE_PLAYING:
            return "助手正在说话";
        case APP_UI_STATE_ERROR:
            return "打开调试页查看";
        case APP_UI_STATE_OFFLINE:
            return "打开网络页重连";
        case APP_UI_STATE_IDLE:
        default:
            return "点击对话开始";
    }
}

static const char *mcp_status_zh(const char *status)
{
    if (!status || status[0] == '\0') {
        return "未知";
    }
    if (strcmp(status, "MCP CONFIGURED") == 0) {
        return "MCP 已配置";
    }
    if (strcmp(status, "WIFI CONNECT") == 0) {
        return "Wi-Fi 连接中";
    }
    if (strcmp(status, "WIFI OK") == 0) {
        return "Wi-Fi 正常";
    }
    if (strcmp(status, "WIFI RETRY") == 0) {
        return "Wi-Fi 重试";
    }
    if (strcmp(status, "MCP CONNECTING") == 0) {
        return "MCP 连接中";
    }
    if (strcmp(status, "MCP CONNECTED") == 0) {
        return "MCP 已连接";
    }
    if (strcmp(status, "MCP DISCONNECTED") == 0) {
        return "MCP 已断开";
    }
    if (strcmp(status, "MCP ERROR") == 0) {
        return "MCP 错误";
    }
    if (strcmp(status, "MCP NOT CONFIGURED") == 0) {
        return "MCP 未配置";
    }
    if (strcmp(status, "MCP SEND") == 0) {
        return "MCP 发送中";
    }
    if (strcmp(status, "MCP SEND FAIL") == 0) {
        return "MCP 发送失败";
    }
    return status;
}

static const char *voice_state_zh(const char *state)
{
    if (!state || state[0] == '\0') {
        return "语音未知";
    }
    if (strcmp(state, "VOICE READY") == 0) {
        return "语音就绪";
    }
    if (strcmp(state, "VOICE LOOP") == 0) {
        return "循环播放";
    }
    if (strcmp(state, "VOICE STOP") == 0) {
        return "播放停止";
    }
    if (strcmp(state, "BT DISABLED") == 0) {
        return "蓝牙未启用";
    }
    if (strcmp(state, "BT TODO") == 0) {
        return "蓝牙待接入";
    }
    return state;
}

static const char *mic_state_zh(const char *state)
{
    if (!state || state[0] == '\0') {
        return "麦克风未知";
    }
    if (strcmp(state, "MIC READY") == 0) {
        return "麦克风就绪";
    }
    if (strcmp(state, "MIC ON") == 0) {
        return "麦克风开启";
    }
    if (strcmp(state, "MIC OFF") == 0) {
        return "麦克风关闭";
    }
    if (strcmp(state, "MIC ERROR") == 0) {
        return "麦克风错误";
    }
    if (strcmp(state, "REC SET") == 0) {
        return "按键录音";
    }
    return state;
}

static lv_color_t state_color(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_ERROR:
        case APP_UI_STATE_OFFLINE:
            return lv_color_hex(UI_ERROR_COLOR);
        case APP_UI_STATE_RECORDING:
        case APP_UI_STATE_PLAYING:
            return lv_color_hex(UI_OK_COLOR);
        case APP_UI_STATE_ASR:
        case APP_UI_STATE_UPLOADING:
            return lv_color_hex(UI_BLUE_COLOR);
        case APP_UI_STATE_TTS:
        case APP_UI_STATE_THINKING:
            return lv_color_hex(UI_ACCENT_COLOR);
        case APP_UI_STATE_IDLE:
        default:
            return lv_color_hex(UI_ACCENT_COLOR);
    }
}

static int state_progress(app_ui_assistant_state_t state)
{
    switch (state) {
        case APP_UI_STATE_RECORDING:
            return 28;
        case APP_UI_STATE_UPLOADING:
            return 42;
        case APP_UI_STATE_ASR:
            return 55;
        case APP_UI_STATE_THINKING:
            return 68;
        case APP_UI_STATE_TTS:
            return 82;
        case APP_UI_STATE_PLAYING:
        case APP_UI_STATE_ERROR:
        case APP_UI_STATE_OFFLINE:
            return 100;
        case APP_UI_STATE_IDLE:
        default:
            return 12;
    }
}

static size_t utf8_char_len(unsigned char ch)
{
    if ((ch & 0x80) == 0) {
        return 1;
    }
    if ((ch & 0xE0) == 0xC0) {
        return 2;
    }
    if ((ch & 0xF0) == 0xE0) {
        return 3;
    }
    if ((ch & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void copy_limited_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && n + 1 < dst_size) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            dst[n++] = ' ';
            ++p;
            continue;
        }
        size_t char_len = utf8_char_len(*p);
        if (n + char_len >= dst_size) {
            break;
        }
        bool valid = true;
        for (size_t i = 1; i < char_len; ++i) {
            if ((p[i] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            dst[n++] = (char)*p++;
            continue;
        }
        for (size_t i = 0; i < char_len; ++i) {
            dst[n++] = (char)*p++;
        }
    }
    dst[n] = '\0';
}

static void format_clock(char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    time_t now = 0;
    struct tm tm_info = {0};
    time(&now);
    localtime_r(&now, &tm_info);
    if (tm_info.tm_year >= 120) {
        strftime(dst, dst_size, "%H:%M", &tm_info);
        return;
    }
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(dst, dst_size, "U%02u:%02u", (unsigned)(uptime_s / 60U), (unsigned)(uptime_s % 60U));
}

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_LVGL_TICK_MS);
}

static void lv_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)drv;
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(&s_disp_drv);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int16_t clamp_coord(int value, int max_value)
{
    if (value < 0) {
        return 0;
    }
    if (value > max_value) {
        return (int16_t)max_value;
    }
    return (int16_t)value;
}

static bool touch_read_bytes(uint8_t *data, size_t len)
{
    if (!s_touch_dev || !data || len == 0) {
        return false;
    }
    return i2c_master_receive(s_touch_dev, data, len, UI_TOUCH_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool touch_read_reg16(uint16_t reg, uint8_t *data, size_t len)
{
    if (!s_touch_dev || !data || len == 0) {
        return false;
    }
    uint8_t reg_buf[2] = {
        (uint8_t)((reg >> 8) & 0xFF),
        (uint8_t)(reg & 0xFF),
    };
    return i2c_master_transmit_receive(s_touch_dev, reg_buf, sizeof(reg_buf), data, len,
                                       UI_TOUCH_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool touch_write_reg16(uint16_t reg, uint8_t value)
{
    if (!s_touch_dev) {
        return false;
    }
    uint8_t payload[3] = {
        (uint8_t)((reg >> 8) & 0xFF),
        (uint8_t)(reg & 0xFF),
        value,
    };
    return i2c_master_transmit(s_touch_dev, payload, sizeof(payload), UI_TOUCH_I2C_TIMEOUT_MS) == ESP_OK;
}

static void map_touch_point(uint16_t raw_x, uint16_t raw_y, int16_t *out_x, int16_t *out_y)
{
    int x = raw_x;
    int y = raw_y;
    int width = UI_W;
    int height = UI_H;

    if (UI_TOUCH_SWAP_XY) {
        int tmp = x;
        x = y;
        y = tmp;
        tmp = width;
        width = height;
        height = tmp;
    }
    x = clamp_coord(x, width - 1);
    y = clamp_coord(y, height - 1);
    if (UI_TOUCH_MIRROR_X) {
        x = width - 1 - x;
    }
    if (UI_TOUCH_MIRROR_Y) {
        y = height - 1 - y;
    }
    if (width != UI_W) {
        x = (x * (UI_W - 1)) / (width - 1);
    }
    if (height != UI_H) {
        y = (y * (UI_H - 1)) / (height - 1);
    }
    *out_x = clamp_coord(x, UI_W - 1);
    *out_y = clamp_coord(y, UI_H - 1);
}

static bool touch_release_after_grace(TickType_t now)
{
    if (!s_touch_pressed) {
        return true;
    }
    if (now - s_last_touch_tick < pdMS_TO_TICKS(UI_TOUCH_RELEASE_GRACE_MS)) {
        return true;
    }
    s_touch_pressed = false;
    return true;
}

static bool poll_tt21100_touch(void)
{
    const TickType_t now = xTaskGetTickCount();
    uint8_t length_buf[2] = {0};
    if (!touch_read_bytes(length_buf, sizeof(length_buf))) {
        if (++s_touch_fail_count == 1 || s_touch_fail_count == 4) {
            ESP_LOGW(TAG, "TT21100 read failed, failures=%u", (unsigned)s_touch_fail_count);
        }
        return touch_release_after_grace(now);
    }

    s_touch_fail_count = 0;
    uint16_t data_len = read_le16(length_buf);
    if (data_len == 0) {
        return touch_release_after_grace(now);
    }
    if (data_len == 14) {
        uint8_t button_report[14];
        touch_read_bytes(button_report, sizeof(button_report));
        return touch_release_after_grace(now);
    }
    if (data_len < UI_TOUCH_REPORT_HEADER_LEN || data_len > UI_TOUCH_REPORT_MAX_LEN) {
        ESP_LOGD(TAG, "TT21100 unexpected report len=%u", (unsigned)data_len);
        return touch_release_after_grace(now);
    }

    uint8_t report[UI_TOUCH_REPORT_MAX_LEN] = {0};
    if (!touch_read_bytes(report, data_len)) {
        return touch_release_after_grace(now);
    }

    uint8_t record_count = (uint8_t)((report[4] >> 3) & 0x1F);
    uint8_t max_records = (uint8_t)((data_len - UI_TOUCH_REPORT_HEADER_LEN) / UI_TOUCH_RECORD_LEN);
    if (record_count == 0 || max_records == 0) {
        return touch_release_after_grace(now);
    }

    const uint8_t *record = report + UI_TOUCH_REPORT_HEADER_LEN;
    int16_t x = 0;
    int16_t y = 0;
    map_touch_point(read_le16(record + 2), read_le16(record + 4), &x, &y);
    s_touch_x = x;
    s_touch_y = y;
    s_touch_pressed = true;
    s_last_touch_tick = now;
    return true;
}

static bool poll_gt911_touch(void)
{
    const TickType_t now = xTaskGetTickCount();
    uint8_t status = 0;
    if (!touch_read_reg16(UI_TOUCH_GT911_REG_STATUS, &status, 1)) {
        if (++s_touch_fail_count == 1 || s_touch_fail_count == 4) {
            ESP_LOGW(TAG, "GT911 read failed, failures=%u", (unsigned)s_touch_fail_count);
        }
        return touch_release_after_grace(now);
    }

    s_touch_fail_count = 0;
    if ((status & 0x80) == 0) {
        return touch_release_after_grace(now);
    }

    uint8_t count = (uint8_t)(status & 0x0F);
    if (count == 0 || count > UI_TOUCH_GT911_MAX_POINTS) {
        touch_write_reg16(UI_TOUCH_GT911_REG_STATUS, 0x00);
        return touch_release_after_grace(now);
    }

    uint8_t point[UI_TOUCH_GT911_POINT_RECORD_LEN] = {0};
    if (!touch_read_reg16(UI_TOUCH_GT911_REG_FIRST_POINT, point, sizeof(point))) {
        return touch_release_after_grace(now);
    }
    touch_write_reg16(UI_TOUCH_GT911_REG_STATUS, 0x00);

    uint16_t raw_x = (uint16_t)(point[1] | ((uint16_t)point[2] << 8));
    uint16_t raw_y = (uint16_t)(point[3] | ((uint16_t)point[4] << 8));
    int16_t x = 0;
    int16_t y = 0;
    map_touch_point(raw_x, raw_y, &x, &y);
    s_touch_x = x;
    s_touch_y = y;
    s_touch_pressed = true;
    s_last_touch_tick = now;
    return true;
}

static esp_err_t add_touch_i2c_device(i2c_master_bus_handle_t i2c_handle, uint8_t addr)
{
    if (s_touch_dev) {
        i2c_master_bus_rm_device(s_touch_dev);
        s_touch_dev = NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = UI_TOUCH_I2C_CLK,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_handle, &dev_cfg, &s_touch_dev);
    if (ret == ESP_OK) {
        s_touch_addr = addr;
    }
    return ret;
}

static bool gt911_identity_is_valid(uint8_t addr, uint8_t product_id[4], uint8_t config_block[5])
{
    if (!touch_read_reg16(UI_TOUCH_GT911_REG_PRODUCT_ID, product_id, 4)) {
        ESP_LOGW(TAG, "GT911 probe 0x%02X product id read failed", addr);
        return false;
    }
    if (!touch_read_reg16(UI_TOUCH_GT911_REG_CONFIG_VERSION, config_block, 5)) {
        ESP_LOGW(TAG, "GT911 probe 0x%02X config read failed", addr);
        return false;
    }

    bool id_ok = product_id[0] == '9' && product_id[1] == '1' && product_id[2] == '1';
    uint16_t raw_w = read_le16(config_block + 1);
    uint16_t raw_h = read_le16(config_block + 3);
    bool size_ok = raw_w >= 64 && raw_w <= 2048 && raw_h >= 64 && raw_h <= 2048;
    if (!id_ok || !size_ok) {
        ESP_LOGW(TAG, "GT911 probe 0x%02X invalid id=%02X %02X %02X %02X raw=%ux%u",
                 addr,
                 (unsigned)product_id[0],
                 (unsigned)product_id[1],
                 (unsigned)product_id[2],
                 (unsigned)product_id[3],
                 (unsigned)raw_w,
                 (unsigned)raw_h);
        return false;
    }
    return true;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (!s_touch_ready || !data) {
        if (data) {
            data->state = LV_INDEV_STATE_REL;
        }
        return;
    }

    if (s_touch_is_gt911) {
        poll_gt911_touch();
    } else {
        poll_tt21100_touch();
    }
    if (s_touch_pressed) {
        data->point.x = s_touch_x;
        data->point.y = s_touch_y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static esp_err_t init_touch(void)
{
    i2c_master_bus_handle_t i2c_handle = i2c_bus_get_master_handle(I2C_NUM_0);
    if (!i2c_handle) {
        ESP_LOGW(TAG, "touch skipped: i2c master handle not ready");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t probe_14 = i2c_master_probe(i2c_handle, 0x14, 100);
    esp_err_t probe_24 = i2c_master_probe(i2c_handle, UI_TOUCH_TT21100_ADDR, 100);
    esp_err_t probe_5d = i2c_master_probe(i2c_handle, 0x5D, 100);
    ESP_LOGI(TAG, "touch i2c scan: 0x14=%s 0x24=%s 0x5D=%s",
             probe_14 == ESP_OK ? "ACK" : "NAK",
             probe_24 == ESP_OK ? "ACK" : "NAK",
             probe_5d == ESP_OK ? "ACK" : "NAK");

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    const char *controller = NULL;
    if (probe_24 == ESP_OK) {
        ret = add_touch_i2c_device(i2c_handle, UI_TOUCH_TT21100_ADDR);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TT21100 touch init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_touch_is_gt911 = false;
        controller = "TT21100";
    } else {
        uint8_t gt911_addrs[] = {UI_TOUCH_GT911_ADDR_BACKUP, UI_TOUCH_GT911_ADDR};
        for (size_t i = 0; i < sizeof(gt911_addrs); ++i) {
            uint8_t addr = gt911_addrs[i];
            if (i2c_master_probe(i2c_handle, addr, 100) != ESP_OK) {
                continue;
            }
            ret = add_touch_i2c_device(i2c_handle, addr);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "GT911 touch device add 0x%02X failed: %s", addr, esp_err_to_name(ret));
                continue;
            }
            uint8_t product_id[4] = {0};
            uint8_t config_block[5] = {0};
            if (!gt911_identity_is_valid(addr, product_id, config_block)) {
                continue;
            }
            s_touch_is_gt911 = true;
            controller = "GT911";
            ESP_LOGI(TAG, "GT911 ready addr=0x%02X id=%02X%02X%02X%02X raw=%ux%u cfg=%u",
                     addr,
                     product_id[0],
                     product_id[1],
                     product_id[2],
                     product_id[3],
                     (unsigned)read_le16(config_block + 1),
                     (unsigned)read_le16(config_block + 3),
                     (unsigned)config_block[0]);
            break;
        }
    }
    if (!controller) {
        ESP_LOGW(TAG, "supported touch controller not found");
        return ESP_ERR_NOT_FOUND;
    }
    s_touch_ready = true;

    lv_indev_drv_init(&s_touch_drv);
    s_touch_drv.type = LV_INDEV_TYPE_POINTER;
    s_touch_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_touch_drv);
    ESP_LOGI(TAG, "%s touch input ready: addr=0x%02X swap=%d mirror=(%d,%d)",
             controller, s_touch_addr, UI_TOUCH_SWAP_XY, UI_TOUCH_MIRROR_X, UI_TOUCH_MIRROR_Y);
    return ESP_OK;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static lv_obj_t *make_wrap_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = make_label(parent, font, color);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int size)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static lv_obj_t *make_pill(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, w, 24);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_radius(pill, 12, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    return pill;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void button_style(lv_obj_t *btn, lv_color_t bg, lv_color_t border)
{
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, border, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

static void page_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    uint8_t page = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    set_page_locked(page);
}

static void action_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    app_ui_action_t action = (app_ui_action_t)(uintptr_t)lv_event_get_user_data(event);
    emit_action(action);
}

static void talk_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        emit_action(APP_UI_ACTION_TALK_PRESS);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        emit_action(APP_UI_ACTION_TALK_RELEASE);
    }
}

static lv_obj_t *make_text_button(lv_obj_t *parent,
                                  int x,
                                  int y,
                                  int w,
                                  int h,
                                  const char *text,
                                  lv_color_t bg,
                                  lv_color_t text_color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    button_style(btn, bg, lv_color_hex(UI_BORDER_COLOR));

    lv_obj_t *label = make_label(btn, UI_FONT_TEXT, text_color);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, w - 10);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *make_app_button(lv_obj_t *parent, int x, int y, const char *icon, const char *name, uint8_t page)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_set_pos(btn, x, y);
    button_style(btn, lv_color_hex(UI_PANEL_COLOR), lv_color_hex(UI_BORDER_COLOR));
    lv_obj_add_event_cb(btn, page_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    lv_obj_t *icon_label = make_label(btn, UI_FONT_CJK, lv_color_hex(UI_ACCENT_COLOR));
    lv_label_set_text(icon_label, icon);
    lv_obj_set_width(icon_label, 54);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(icon_label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *name_label = make_label(btn, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_label_set_text(name_label, name);
    lv_obj_set_width(name_label, 58);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name_label, LV_ALIGN_BOTTOM_MID, 0, -7);
    return btn;
}

static lv_obj_t *make_page(lv_obj_t *screen)
{
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, UI_W, UI_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static void set_page_locked(uint8_t page)
{
    if (page >= UI_PAGE_COUNT) {
        page = 0;
    }
    bool initial_layout = s_page_dirty;
    if (s_page == page && !initial_layout) {
        return;
    }
    uint8_t prev = s_page;
    s_page = page;
    for (uint8_t i = 0; i < UI_PAGE_COUNT; ++i) {
        if (s_pages[i]) {
            if (i == s_page) {
                lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_page_dots[i]) {
            lv_obj_set_style_bg_color(
                s_page_dots[i],
                i == s_page ? lv_color_hex(UI_ACCENT_COLOR) : lv_color_hex(0xE9CBD9),
                0);
        }
    }
    if (s_page_label) {
        lv_label_set_text_fmt(s_page_label, "%s %u/%u", page_title(s_page), (unsigned)s_page + 1, UI_PAGE_COUNT);
    }
    if (prev != s_page || initial_layout) {
        ESP_LOGI(TAG, "ui page=%s %u/%u", page_title(s_page), (unsigned)s_page + 1, UI_PAGE_COUNT);
    }
    s_page_dirty = false;
    if (prev != s_page || initial_layout) {
        s_dirty = true;
    }
}

static void change_page_locked(int delta)
{
    int next = (int)s_page + delta;
    if (next < 0) {
        next = UI_PAGE_COUNT - 1;
    } else if (next >= UI_PAGE_COUNT) {
        next = 0;
    }
    set_page_locked((uint8_t)next);
}

static void nav_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t delta = (intptr_t)lv_event_get_user_data(event);
    change_page_locked((int)delta);
}

static void screen_gesture_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        change_page_locked(1);
    } else if (dir == LV_DIR_RIGHT) {
        change_page_locked(-1);
    }
}

static lv_obj_t *make_nav_button(lv_obj_t *screen, int x, const char *text, int delta)
{
    lv_obj_t *btn = lv_btn_create(screen);
    lv_obj_set_size(btn, 28, 72);
    lv_obj_set_pos(btn, x, 88);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, nav_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)delta);

    lv_obj_t *label = make_label(btn, &lv_font_montserrat_24, lv_color_hex(UI_MUTED_COLOR));
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void create_top_bar(lv_obj_t *screen)
{
    lv_obj_t *wifi_pill = make_pill(screen, 8, 8, 72);
    s_wifi_dot = make_dot(wifi_pill, 8);
    lv_obj_align(s_wifi_dot, LV_ALIGN_LEFT_MID, 9, 0);
    s_wifi_label = make_label(wifi_pill, &lv_font_montserrat_14, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_width(s_wifi_label, 44);
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t *mcp_pill = make_pill(screen, 86, 8, 66);
    s_mcp_dot = make_dot(mcp_pill, 8);
    lv_obj_align(s_mcp_dot, LV_ALIGN_LEFT_MID, 9, 0);
    s_mcp_label = make_label(mcp_pill, &lv_font_montserrat_14, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_width(s_mcp_label, 38);
    lv_obj_align(s_mcp_label, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t *vol_pill = make_pill(screen, UI_W - 82, 8, 74);
    s_volume_label = make_label(vol_pill, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_width(s_volume_label, 64);
    lv_obj_center(s_volume_label);

    s_page_label = make_label(screen, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_width(s_page_label, UI_W - 16);
    lv_obj_set_pos(s_page_label, 8, 38);
    lv_obj_set_style_text_align(s_page_label, LV_TEXT_ALIGN_RIGHT, 0);
}

static void create_home_page(lv_obj_t *page)
{
    s_home_time_label = make_label(page, &lv_font_montserrat_32, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_width(s_home_time_label, 120);
    lv_obj_set_pos(s_home_time_label, 16, 48);

    s_home_status_label = make_wrap_label(page, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_size(s_home_status_label, UI_W - 154, 40);
    lv_obj_set_pos(s_home_status_label, 142, 52);

    make_app_button(page, 18, 108, "聊", "对话", 1);
    make_app_button(page, 94, 108, "网", "网络", 2);
    make_app_button(page, 170, 108, "设", "设置", 3);
    make_app_button(page, 246, 108, "调", "调试", 4);

    lv_obj_t *quick = make_text_button(page,
                                       18,
                                       188,
                                       UI_W - 36,
                                       38,
                                       "按住说话",
                                       lv_color_hex(UI_ACCENT_SOFT_COLOR),
                                       lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(quick, talk_button_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(quick, talk_button_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(quick, talk_button_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void create_chat_page(lv_obj_t *page)
{
    lv_obj_t *title = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_label_set_text(title, "对话");
    lv_obj_set_pos(title, 16, 44);

    s_state_label = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_ACCENT_COLOR));
    lv_obj_set_width(s_state_label, UI_W - 64);
    lv_obj_set_pos(s_state_label, 32, 74);
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);

    s_state_caption_label = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_width(s_state_caption_label, UI_W - 64);
    lv_obj_set_pos(s_state_caption_label, 32, 112);
    lv_obj_set_style_text_align(s_state_caption_label, LV_TEXT_ALIGN_CENTER, 0);

    s_status_track = lv_obj_create(page);
    lv_obj_set_size(s_status_track, UI_W - 72, 7);
    lv_obj_set_pos(s_status_track, 36, 138);
    lv_obj_set_style_radius(s_status_track, 4, 0);
    lv_obj_set_style_border_width(s_status_track, 0, 0);
    lv_obj_set_style_bg_color(s_status_track, lv_color_hex(0xF4DCE8), 0);
    lv_obj_set_style_bg_opa(s_status_track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_status_track, LV_OBJ_FLAG_SCROLLABLE);

    s_status_line = lv_obj_create(page);
    lv_obj_set_size(s_status_line, 24, 7);
    lv_obj_set_pos(s_status_line, 36, 138);
    lv_obj_set_style_radius(s_status_line, 4, 0);
    lv_obj_set_style_border_width(s_status_line, 0, 0);
    lv_obj_set_style_bg_opa(s_status_line, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_status_line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *recent_card = make_card(page, 18, 154, UI_W - 36, 36);
    s_chat_in_label = make_wrap_label(recent_card, UI_FONT_RECENT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_chat_in_label, UI_W - 54, 22);
    lv_obj_align(s_chat_in_label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_recent_label = s_chat_in_label;

    lv_obj_t *reply_card = make_card(page, 18, 194, UI_W - 126, 32);
    lv_obj_set_style_bg_color(reply_card, lv_color_hex(UI_SOFT_PANEL_COLOR), 0);
    s_chat_out_label = make_wrap_label(reply_card, UI_FONT_RECENT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_chat_out_label, UI_W - 144, 18);
    lv_obj_align(s_chat_out_label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_reply_label = s_chat_out_label;

    s_talk_button = make_text_button(page,
                                     UI_W - 96,
                                     194,
                                     78,
                                     32,
                                     "按住",
                                     lv_color_hex(UI_ACCENT_SOFT_COLOR),
                                     lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(s_talk_button, talk_button_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_talk_button, talk_button_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_talk_button, talk_button_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_talk_button_label = lv_obj_get_child(s_talk_button, 0);

    s_chat_system_label = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_size(s_chat_system_label, UI_W - 36, 16);
    lv_obj_set_pos(s_chat_system_label, 18, 229);
}

static void create_network_page(lv_obj_t *page)
{
    lv_obj_t *title = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_label_set_text(title, "网络");
    lv_obj_set_pos(title, 16, 44);

    lv_obj_t *wifi_card = make_card(page, 18, 78, UI_W - 36, 38);
    s_net_wifi_label = make_label(wifi_card, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_net_wifi_label, UI_W - 54, 18);
    lv_obj_align(s_net_wifi_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *mcp_card = make_card(page, 18, 122, UI_W - 36, 38);
    s_net_mcp_label = make_label(mcp_card, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_net_mcp_label, UI_W - 54, 18);
    lv_obj_align(s_net_mcp_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *endpoint_card = make_card(page, 18, 166, UI_W - 36, 28);
    s_net_endpoint_label = make_label(endpoint_card, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_size(s_net_endpoint_label, UI_W - 54, 16);
    lv_obj_align(s_net_endpoint_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *connect_btn = make_text_button(page, 18, 204, 92, 32, "连接", lv_color_hex(UI_ACCENT_SOFT_COLOR),
                                             lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(connect_btn, action_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)APP_UI_ACTION_MCP_CONNECT);

    lv_obj_t *reconnect_btn = make_text_button(page, 116, 204, 92, 32, "重连", lv_color_hex(UI_PANEL_COLOR),
                                               lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(reconnect_btn, action_button_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APP_UI_ACTION_MCP_RECONNECT);

    lv_obj_t *off_btn = make_text_button(page, 214, 204, 88, 32, "离线", lv_color_hex(UI_PANEL_COLOR),
                                         lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(off_btn, action_button_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APP_UI_ACTION_MCP_DISCONNECT);
}

static void create_settings_page(lv_obj_t *page)
{
    lv_obj_t *title = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_label_set_text(title, "设置");
    lv_obj_set_pos(title, 16, 44);

    lv_obj_t *time_card = make_card(page, 18, 78, UI_W - 36, 34);
    s_set_time_label = make_label(time_card, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_set_time_label, UI_W - 54, 18);
    lv_obj_align(s_set_time_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *bt_card = make_card(page, 18, 118, UI_W - 36, 34);
    s_set_bt_label = make_label(bt_card, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(s_set_bt_label, UI_W - 54, 18);
    lv_obj_align(s_set_bt_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(bt_card, action_button_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APP_UI_ACTION_BLUETOOTH_TOGGLE);
    lv_obj_add_flag(bt_card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *minus_btn = make_text_button(page, 18, 164, 64, 34, "音量-", lv_color_hex(UI_PANEL_COLOR),
                                           lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(minus_btn, action_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)APP_UI_ACTION_VOL_DOWN);

    lv_obj_t *plus_btn = make_text_button(page, 92, 164, 64, 34, "音量+", lv_color_hex(UI_PANEL_COLOR),
                                          lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(plus_btn, action_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)APP_UI_ACTION_VOL_UP);

    lv_obj_t *test_btn = make_text_button(page, 166, 164, 136, 34, "测试声音", lv_color_hex(UI_ACCENT_SOFT_COLOR),
                                          lv_color_hex(UI_TEXT_COLOR));
    lv_obj_add_event_cb(test_btn, action_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)APP_UI_ACTION_PLAY_TEST);

    s_set_status_label = make_wrap_label(page, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_size(s_set_status_label, UI_W - 36, 34);
    lv_obj_set_pos(s_set_status_label, 18, 210);
}

static lv_obj_t *make_debug_row(lv_obj_t *page, int y, const char *name, lv_obj_t **value_label, lv_color_t dot_color)
{
    lv_obj_t *row = make_card(page, 18, y, UI_W - 36, 31);
    lv_obj_t *name_label = make_label(row, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_label_set_text(name_label, name);
    lv_obj_set_size(name_label, 52, 16);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 0, 0);

    *value_label = make_label(row, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_obj_set_size(*value_label, UI_W - 124, 18);
    lv_obj_align(*value_label, LV_ALIGN_LEFT_MID, 60, 0);

    lv_obj_t *dot = make_dot(row, 10);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -2, 0);
    return row;
}

static void create_debug_page(lv_obj_t *page)
{
    lv_obj_t *title = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_TEXT_COLOR));
    lv_label_set_text(title, "调试");
    lv_obj_set_pos(title, 16, 44);

    make_debug_row(page, 76, "网络", &s_dbg_network_label, lv_color_hex(UI_OK_COLOR));
    make_debug_row(page, 112, "音频", &s_dbg_audio_label, lv_color_hex(UI_BLUE_COLOR));
    make_debug_row(page, 148, "阶段", &s_dbg_phase_label, lv_color_hex(UI_ACCENT_COLOR));
    make_debug_row(page, 184, "错误", &s_dbg_error_label, lv_color_hex(UI_OK_COLOR));

    s_dbg_mic_label = make_label(page, UI_FONT_TEXT, lv_color_hex(UI_MUTED_COLOR));
    lv_obj_set_size(s_dbg_mic_label, UI_W - 130, 18);
    lv_obj_set_pos(s_dbg_mic_label, 20, 224);

    s_mic_bar = lv_bar_create(page);
    lv_obj_set_size(s_mic_bar, 86, 7);
    lv_obj_set_pos(s_mic_bar, UI_W - 110, 228);
    lv_bar_set_range(s_mic_bar, 0, 22000);
    lv_obj_set_style_radius(s_mic_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_mic_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(0xEBD5E0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(UI_OK_COLOR), LV_PART_INDICATOR);
}

static void create_page_dots(lv_obj_t *screen)
{
    int start_x = (UI_W - (UI_PAGE_COUNT - 1) * 18 - 8) / 2;
    for (uint8_t i = 0; i < UI_PAGE_COUNT; ++i) {
        s_page_dots[i] = make_dot(screen, 8);
        lv_obj_set_pos(s_page_dots[i], start_x + i * 18, UI_H - 12);
    }
}

static void create_ui_objects(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    for (uint8_t i = 0; i < UI_PAGE_COUNT; ++i) {
        s_pages[i] = make_page(screen);
        lv_obj_add_event_cb(s_pages[i], screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    }
    create_home_page(s_pages[0]);
    create_chat_page(s_pages[1]);
    create_network_page(s_pages[2]);
    create_settings_page(s_pages[3]);
    create_debug_page(s_pages[4]);

    create_top_bar(screen);
    make_nav_button(screen, 4, "<", -1);
    make_nav_button(screen, UI_W - 32, ">", 1);
    create_page_dots(screen);
    set_page_locked(0);
}

static void apply_ui_locked(void)
{
    if (!s_ready) {
        return;
    }

    char clock_text[16];
    format_clock(clock_text, sizeof(clock_text));

    lv_label_set_text(s_wifi_label, s_wifi_connected ? "Wi-Fi" : "Wi-Fi");
    lv_label_set_text(s_mcp_label, "MCP");
    lv_obj_set_style_bg_color(s_wifi_dot, s_wifi_connected ? lv_color_hex(UI_OK_COLOR) : lv_color_hex(0xC5AABC), 0);
    lv_obj_set_style_bg_color(s_mcp_dot, s_mcp_connected ? lv_color_hex(UI_OK_COLOR) : lv_color_hex(0xC5AABC), 0);
    lv_label_set_text_fmt(s_volume_label, "音量%02d", s_volume);

    if (s_page == 0 || s_clock_dirty) {
        lv_label_set_text(s_home_time_label, clock_text);
    }
    lv_label_set_text_fmt(s_home_status_label, "%s\n%s / %s",
                          home_state_hint(s_assistant_state),
                          s_wifi_connected ? "Wi-Fi 正常" : "Wi-Fi 断开",
                          s_mcp_connected ? "MCP 正常" : "MCP 断开");

    lv_label_set_text(s_state_label, state_text(s_assistant_state));
    lv_obj_set_style_text_color(s_state_label, state_color(s_assistant_state), 0);
    lv_label_set_text(s_state_caption_label, state_caption(s_assistant_state));
    lv_obj_set_style_bg_color(s_status_line, state_color(s_assistant_state), 0);
    int progress_width = ((UI_W - 72) * state_progress(s_assistant_state)) / 100;
    if (progress_width < 24) {
        progress_width = 24;
    }
    lv_obj_set_width(s_status_line, progress_width);

    const char *recent = s_recent_text[0] ? s_recent_text : "暂无文字";
    lv_label_set_text_fmt(s_chat_in_label, "我：%s", recent);
    lv_label_set_text_fmt(s_chat_out_label, "答：%s", voice_state_zh(s_voice_state));
    lv_label_set_text_fmt(s_chat_system_label, "状态：%s", state_caption(s_assistant_state));
    lv_label_set_text(s_talk_button_label,
                      s_assistant_state == APP_UI_STATE_RECORDING ? "发送" : "按住");
    lv_obj_set_style_bg_color(s_talk_button,
                              s_assistant_state == APP_UI_STATE_RECORDING ? lv_color_hex(UI_OK_COLOR)
                                                                          : lv_color_hex(UI_ACCENT_SOFT_COLOR),
                              0);

    lv_label_set_text_fmt(s_net_wifi_label, "Wi-Fi：%s", s_wifi_connected ? "已连接" : "连接中或离线");
    lv_label_set_text_fmt(s_net_mcp_label, "MCP：%s", mcp_status_zh(s_mcp_status));
    lv_label_set_text(s_net_endpoint_label, "地址：app_config.h 配置");

    if (s_page == 3 || s_clock_dirty) {
        lv_label_set_text_fmt(s_set_time_label, "时间：%s", clock_text);
    }
    lv_label_set_text_fmt(s_set_bt_label, "蓝牙：%s",
                          s_bt_available ? (s_bt_enabled ? "开启" : "关闭") : "固件未启用");
    lv_label_set_text_fmt(s_set_status_label, "音频：16k / 16bit / 单声道  音量%02d",
                          s_volume);

    lv_label_set_text_fmt(s_dbg_network_label, "%s / %s", s_wifi_connected ? "Wi-Fi 正常" : "Wi-Fi 断开",
                          s_mcp_connected ? "MCP 正常" : "MCP 断开");
    lv_label_set_text_fmt(s_dbg_audio_label, "16k / 16bit / 单声道 / 音量%02d", s_volume);
    lv_label_set_text_fmt(s_dbg_phase_label, "当前：%s", state_text(s_assistant_state));
    lv_label_set_text(s_dbg_error_label, s_assistant_state == APP_UI_STATE_ERROR ? mcp_status_zh(s_mcp_status) : "无");
    lv_label_set_text_fmt(s_dbg_mic_label, "%s  峰值%05d 均值%04d", mic_state_zh(s_mic_state), s_mic_peak, s_mic_avg);
    lv_bar_set_value(s_mic_bar, s_mic_peak, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        s_mic_bar,
        s_mic_peak > 12000 ? lv_color_hex(UI_ERROR_COLOR) : lv_color_hex(UI_OK_COLOR),
        LV_PART_INDICATOR);

    if (s_page_dirty) {
        set_page_locked(s_page);
    }
    s_clock_dirty = false;
    s_dirty = false;
}

static void mark_dirty(void)
{
    s_dirty = true;
}

static void emit_action(app_ui_action_t action)
{
    if (!s_action_queue) {
        return;
    }
    if (xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        app_ui_action_t dropped = APP_UI_ACTION_PLAY_TEST;
        xQueueReceive(s_action_queue, &dropped, 0);
        xQueueSend(s_action_queue, &action, 0);
    }
}

static void action_dispatch_task(void *arg)
{
    (void)arg;
    app_ui_action_t action = APP_UI_ACTION_PLAY_TEST;
    while (true) {
        if (xQueueReceive(s_action_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        app_ui_action_cb_t cb = s_action_cb;
        void *ctx = s_action_ctx;
        if (cb) {
            cb(action, ctx);
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(UI_LOOP_INTERVAL_MS));
        if (!s_ready || !s_lock) {
            continue;
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            if (s_mcp_connected && s_last_mcp_activity_tick != 0 &&
                now - s_last_mcp_activity_tick > pdMS_TO_TICKS(UI_MCP_OFFLINE_TIMEOUT_MS)) {
                s_mcp_connected = false;
                s_assistant_state = APP_UI_STATE_OFFLINE;
                mark_dirty();
            }
            if (now - s_last_clock_tick >= pdMS_TO_TICKS(UI_CLOCK_REFRESH_MS)) {
                s_last_clock_tick = now;
                s_clock_dirty = true;
                mark_dirty();
            }
            if (s_dirty && now - s_last_render_tick >= pdMS_TO_TICKS(UI_RENDER_INTERVAL_MS)) {
                s_last_render_tick = now;
                apply_ui_locked();
            }
            lv_timer_handler();
            xSemaphoreGive(s_lock);
        }
    }
}

esp_err_t app_ui_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_action_queue = xQueueCreate(UI_ACTION_QUEUE_LEN, sizeof(app_ui_action_t));
    if (!s_action_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (!s_periph_set) {
        ESP_LOGE(TAG, "esp_periph_set_init failed");
        return ESP_FAIL;
    }

    s_panel = audio_board_lcd_init(s_periph_set, NULL);
    if (!s_panel) {
        ESP_LOGE(TAG, "audio_board_lcd_init failed");
        return ESP_FAIL;
    }

    const size_t pixel_count = UI_W * UI_DRAW_LINES;
    s_buf1 = heap_caps_malloc(pixel_count * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = heap_caps_malloc(pixel_count * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "lvgl buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, pixel_count);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = UI_W;
    s_disp_drv.ver_res = UI_H;
    s_disp_drv.flush_cb = lv_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);
    init_touch();

    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name = "lv_tick",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, UI_LVGL_TICK_MS * 1000));

    create_ui_objects();
    s_ready = true;
    s_dirty = true;
    s_page_dirty = true;
    s_clock_dirty = true;
    apply_ui_locked();
    lv_timer_handler();
    xTaskCreate(ui_task, "app_ui", 4096, NULL, 4, NULL);
    xTaskCreate(action_dispatch_task, "ui_actions", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "ready using LVGL %dx%d, page UI, touch=%s", UI_W, UI_H, s_touch_ready ? "yes" : "no");
    return ESP_OK;
}

void app_ui_set_action_callback(app_ui_action_cb_t cb, void *ctx)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_action_cb = cb;
        s_action_ctx = ctx;
        xSemaphoreGive(s_lock);
        return;
    }
    s_action_cb = cb;
    s_action_ctx = ctx;
}

void app_ui_set_voice_state(const char *state)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_voice_state, state ? state : "VOICE UNKNOWN", sizeof(s_voice_state));
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_mic_level(int peak, int avg_abs)
{
    if (s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        s_mic_peak = peak;
        s_mic_avg = avg_abs;
        xSemaphoreGive(s_lock);
    } else {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (now - s_last_level_render_tick < pdMS_TO_TICKS(UI_LEVEL_REFRESH_MS)) {
        return;
    }
    s_last_level_render_tick = now;
    mark_dirty();
}

void app_ui_set_mic_state(const char *state)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_mic_state, state ? state : "MIC UNKNOWN", sizeof(s_mic_state));
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_mcp_status(const char *status)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_mcp_status, status ? status : "MCP UNKNOWN", sizeof(s_mcp_status));
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_volume(int volume)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_volume = volume;
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_bluetooth_available(bool available)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_bt_available = available;
        if (!available) {
            s_bt_enabled = false;
        }
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_bluetooth_enabled(bool enabled)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_bt_enabled = s_bt_available && enabled;
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_wifi_connected(bool connected)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_wifi_connected = connected;
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_mcp_connected(bool connected)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_mcp_connected = connected;
        if (connected) {
            s_last_mcp_activity_tick = xTaskGetTickCount();
            if (s_assistant_state == APP_UI_STATE_OFFLINE) {
                s_assistant_state = APP_UI_STATE_IDLE;
            }
        } else {
            s_assistant_state = APP_UI_STATE_OFFLINE;
        }
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_note_mcp_activity(void)
{
    s_last_mcp_activity_tick = xTaskGetTickCount();
}

void app_ui_set_assistant_state(app_ui_assistant_state_t state)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_assistant_state = state;
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_set_recent_text(const char *text)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy_limited_text(s_recent_text, sizeof(s_recent_text), text);
        mark_dirty();
        xSemaphoreGive(s_lock);
    }
}

void app_ui_next_page(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        change_page_locked(1);
        xSemaphoreGive(s_lock);
    }
}

void app_ui_prev_page(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        change_page_locked(-1);
        xSemaphoreGive(s_lock);
    }
}
