#include "app_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UI_W LCD_H_RES
#define UI_H LCD_V_RES
#define UI_FLUSH_LINES 16
#define UI_LEVEL_REFRESH_MS 200

#define C_BG 0x0841
#define C_PANEL 0x18E3
#define C_TEXT 0xFFFF
#define C_MUTED 0x7BEF
#define C_GREEN 0x07E0
#define C_BLUE 0x3D7C
#define C_YELLOW 0xFFE0
#define C_RED 0xF800

static const char *TAG = "APP_UI";
static esp_periph_set_handle_t s_periph_set;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;
static uint16_t *s_line_buf;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static char s_voice_state[32] = "VOICE READY";
static char s_mic_state[32] = "MIC OFF";
static char s_mcp_status[48] = "MCP NOT CONFIGURED";
static int s_volume = 80;
static int s_mic_peak;
static int s_mic_avg;
static TickType_t s_last_level_render_tick;

static const uint8_t *glyph_for(char ch)
{
    static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t colon[7] = {0, 4, 4, 0, 4, 4, 0};
    static const uint8_t dash[7] = {0, 0, 0, 31, 0, 0, 0};
    static const uint8_t plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const uint8_t slash[7] = {1, 2, 4, 8, 16, 0, 0};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 12, 12};
    static const uint8_t qmark[7] = {14, 17, 1, 2, 4, 0, 4};
    static const uint8_t nums[10][7] = {
        {14, 17, 19, 21, 25, 17, 14},
        {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2},
        {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14},
        {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 2, 12},
    };
    static const uint8_t letters[26][7] = {
        {14, 17, 17, 31, 17, 17, 17},
        {30, 17, 17, 30, 17, 17, 30},
        {14, 17, 16, 16, 16, 17, 14},
        {30, 17, 17, 17, 17, 17, 30},
        {31, 16, 16, 30, 16, 16, 31},
        {31, 16, 16, 30, 16, 16, 16},
        {14, 17, 16, 23, 17, 17, 15},
        {17, 17, 17, 31, 17, 17, 17},
        {14, 4, 4, 4, 4, 4, 14},
        {7, 2, 2, 2, 18, 18, 12},
        {17, 18, 20, 24, 20, 18, 17},
        {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17},
        {17, 25, 21, 19, 17, 17, 17},
        {14, 17, 17, 17, 17, 17, 14},
        {30, 17, 17, 30, 16, 16, 16},
        {14, 17, 17, 17, 21, 18, 13},
        {30, 17, 17, 30, 20, 18, 17},
        {15, 16, 16, 14, 1, 1, 30},
        {31, 4, 4, 4, 4, 4, 4},
        {17, 17, 17, 17, 17, 17, 14},
        {17, 17, 17, 17, 17, 10, 4},
        {17, 17, 17, 21, 21, 21, 10},
        {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 10, 4, 4, 4, 4},
        {31, 1, 2, 4, 8, 16, 31},
    };

    if (ch >= '0' && ch <= '9') {
        return nums[ch - '0'];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return letters[ch - 'A'];
    }
    switch (ch) {
        case ' ':
            return space;
        case ':':
            return colon;
        case '-':
            return dash;
        case '+':
            return plus;
        case '/':
            return slash;
        case '.':
            return dot;
        default:
            return qmark;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_frame) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > UI_W) {
        w = UI_W - x;
    }
    if (y + h > UI_H) {
        h = UI_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = y; row < y + h; ++row) {
        uint16_t *dst = s_frame + row * UI_W + x;
        for (int col = 0; col < w; ++col) {
            dst[col] = color;
        }
    }
}

static void draw_char(int x, int y, char ch, uint16_t color, int scale)
{
    const uint8_t *glyph = glyph_for(ch);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (glyph[row] & (1 << (4 - col))) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint16_t color, int scale)
{
    int cursor = x;
    for (const char *p = text; p && *p; ++p) {
        draw_char(cursor, y, *p, color, scale);
        cursor += 6 * scale;
    }
}

static void flush_frame(void)
{
    if (!s_panel || !s_frame || !s_line_buf) {
        return;
    }

    for (int y = 0; y < UI_H; y += UI_FLUSH_LINES) {
        int lines = UI_FLUSH_LINES;
        if (y + lines > UI_H) {
            lines = UI_H - y;
        }
        memcpy(s_line_buf, s_frame + y * UI_W, UI_W * lines * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, UI_W, y + lines, s_line_buf);
    }
}

static void render_locked(void)
{
    if (!s_ready) {
        return;
    }

    fill_rect(0, 0, UI_W, UI_H, C_BG);
    fill_rect(0, 0, UI_W, 44, C_BLUE);
    draw_text(12, 12, "XIAOLE ROBOT", C_TEXT, 2);

    fill_rect(10, 58, UI_W - 20, 42, C_PANEL);
    draw_text(20, 72, s_voice_state, C_GREEN, 2);

    fill_rect(10, 110, UI_W - 20, 42, C_PANEL);
    draw_text(20, 124, s_mic_state, C_TEXT, 2);

    int mic_bar = s_mic_peak / 140;
    if (mic_bar > UI_W - 40) {
        mic_bar = UI_W - 40;
    }
    fill_rect(20, 160, UI_W - 40, 16, C_MUTED);
    fill_rect(20, 160, mic_bar, 16, s_mic_peak > 12000 ? C_RED : C_GREEN);

    char line[48];
    snprintf(line, sizeof(line), "PEAK %05d AVG %04d", s_mic_peak, s_mic_avg);
    draw_text(20, 184, line, C_YELLOW, 1);

    snprintf(line, sizeof(line), "VOL %03d", s_volume);
    draw_text(20, 204, line, C_TEXT, 1);
    draw_text(120, 204, s_mcp_status, C_MUTED, 1);

    flush_frame();
}

static void render(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    render_locked();
    xSemaphoreGive(s_lock);
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

    s_frame = heap_caps_malloc(UI_W * UI_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        s_frame = heap_caps_malloc(UI_W * UI_H * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    s_line_buf = heap_caps_malloc(UI_W * UI_FLUSH_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_frame || !s_line_buf) {
        ESP_LOGE(TAG, "lcd buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    s_ready = true;
    render();
    ESP_LOGI(TAG, "ready using board lcd config %dx%d", UI_W, UI_H);
    return ESP_OK;
}

void app_ui_set_voice_state(const char *state)
{
    strlcpy(s_voice_state, state ? state : "VOICE UNKNOWN", sizeof(s_voice_state));
    render();
}

void app_ui_set_mic_level(int peak, int avg_abs)
{
    s_mic_peak = peak;
    s_mic_avg = avg_abs;
    TickType_t now = xTaskGetTickCount();
    if (now - s_last_level_render_tick < pdMS_TO_TICKS(UI_LEVEL_REFRESH_MS)) {
        return;
    }
    s_last_level_render_tick = now;
    render();
}

void app_ui_set_mic_state(const char *state)
{
    strlcpy(s_mic_state, state ? state : "MIC UNKNOWN", sizeof(s_mic_state));
    render();
}

void app_ui_set_mcp_status(const char *status)
{
    strlcpy(s_mcp_status, status ? status : "MCP UNKNOWN", sizeof(s_mcp_status));
    render();
}

void app_ui_set_volume(int volume)
{
    s_volume = volume;
    render();
}
