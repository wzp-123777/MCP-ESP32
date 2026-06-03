#include "music_player.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "audio_mem.h"
#include "audio_player.h"
#include "board.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MUSIC_ROOT_PRIMARY "/sdcard/music"
#define MUSIC_ROOT_FALLBACK "/sdcard"
#define MUSIC_MAX_TRACKS 64
#define MUSIC_PATH_MAX 192
#define MUSIC_CMD_QUEUE_LEN 8
#define MUSIC_TASK_STACK 5120
#define MUSIC_STREAM_CHUNK_BYTES 4096
#define MUSIC_OUTPUT_RATE 16000
#define MUSIC_OUTPUT_CHANNELS 1
#define MUSIC_OUTPUT_BITS 16
#define MUSIC_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSIC_QUEUE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSIC_STREAM_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSIC_SD_FAIL_COOLDOWN_MS 30000
#define MUSIC_INVALID_INDEX (-1)

typedef enum {
    MUSIC_CMD_PLAY = 0,
    MUSIC_CMD_STOP,
    MUSIC_CMD_NEXT,
    MUSIC_CMD_PREV,
    MUSIC_CMD_REFRESH,
    MUSIC_CMD_SELECT,
} music_cmd_id_t;

typedef struct {
    music_cmd_id_t id;
    int index;
} music_cmd_t;

typedef struct {
    FILE *file;
    uint32_t data_remaining;
    bool bounded;
} music_file_stream_t;

static const char *TAG = "MUSIC_PLAYER";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_queue;
static esp_periph_set_handle_t s_periph_set;
static bool s_mounted;
static bool s_playing;
static bool s_stop_requested;
static bool s_next_requested;
static bool s_prev_requested;
static bool s_select_requested;
static int s_select_index = MUSIC_INVALID_INDEX;
static bool s_sd_fail_latched;
static TickType_t s_sd_retry_after_tick;
static esp_err_t s_last_sd_error = ESP_OK;
static uint32_t s_sd_fail_count;
static char s_tracks[MUSIC_MAX_TRACKS][MUSIC_PATH_MAX];
static int s_track_count;
static int s_track_index;
static char s_status[MUSIC_PLAYER_STATUS_MAX] = "MUSIC IDLE";
static music_player_state_cb_t s_state_cb;
static void *s_state_ctx;

static void notify_state(void);

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool has_extension(const char *name, const char *ext)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && ext && strcasecmp(dot, ext) == 0;
}

static bool is_prepared_audio_file(const char *name)
{
    return has_extension(name, ".wav") || has_extension(name, ".pcm");
}

static int visible_start_for_index(int track_index, int track_count)
{
    int start = track_index - 1;
    if (start < 0) {
        start = 0;
    }
    if (track_count > MUSIC_PLAYER_LIST_LINES && start > track_count - MUSIC_PLAYER_LIST_LINES) {
        start = track_count - MUSIC_PLAYER_LIST_LINES;
    }
    return start;
}

static void copy_track_title(const char *path, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    const char *name = path && path[0] ? strrchr(path, '/') : NULL;
    if (!name) {
        name = path && path[0] ? strrchr(path, '\\') : NULL;
    }
    name = name ? name + 1 : path;
    if (!name || !name[0]) {
        strlcpy(dst, "NO TRACK", dst_size);
        return;
    }
    strlcpy(dst, name, dst_size);
    char *dot = strrchr(dst, '.');
    if (dot && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".pcm") == 0)) {
        *dot = '\0';
    }
}

static void set_status(const char *status)
{
    if (!status) {
        return;
    }
    bool changed = true;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    changed = strcmp(s_status, status) != 0;
    strlcpy(s_status, status, sizeof(s_status));
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (changed) {
        ESP_LOGI(TAG, "%s", status);
    }
    notify_state();
}

const char *music_player_get_status(void)
{
    return s_status;
}

bool music_player_is_playing(void)
{
    bool playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    playing = s_playing;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return playing;
}

void music_player_get_state(music_player_state_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    out->mounted = s_mounted;
    out->playing = s_playing;
    out->track_count = s_track_count;
    out->track_index = s_track_index;
    strlcpy(out->status, s_status, sizeof(out->status));
    if (s_track_count > 0 && s_track_index >= 0 && s_track_index < s_track_count) {
        copy_track_title(s_tracks[s_track_index], out->title, sizeof(out->title));
    } else {
        strlcpy(out->title, s_mounted ? "NO WAV/PCM FOUND" : "SCAN TF CARD", sizeof(out->title));
    }

    int start = visible_start_for_index(s_track_index, s_track_count);
    for (int i = 0; i < MUSIC_PLAYER_LIST_LINES; ++i) {
        int idx = start + i;
        if (idx >= 0 && idx < s_track_count) {
            char title[48];
            copy_track_title(s_tracks[idx], title, sizeof(title));
            snprintf(out->list[i], sizeof(out->list[i]), "%c %02d %s",
                     idx == s_track_index ? '>' : ' ',
                     idx + 1,
                     title);
        } else if (i == 0 && s_track_count <= 0) {
            strlcpy(out->list[i], "Put 16k mono WAV in /music", sizeof(out->list[i]));
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void notify_state(void)
{
    music_player_state_cb_t cb = s_state_cb;
    void *ctx = s_state_ctx;
    if (!cb) {
        return;
    }
    music_player_state_t state;
    music_player_get_state(&state);
    cb(&state, ctx);
}

static bool request_stop_locked(int step)
{
    bool was_playing = s_playing;
    s_stop_requested = true;
    if (step > 0) {
        s_next_requested = true;
    } else if (step < 0) {
        s_prev_requested = true;
    }
    return was_playing;
}

static bool request_select_locked(int index)
{
    bool was_playing = s_playing;
    s_select_requested = true;
    s_select_index = index;
    s_stop_requested = was_playing;
    return was_playing;
}

static void request_stop(int step)
{
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    was_playing = request_stop_locked(step);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (was_playing) {
        audio_player_cancel();
    }
}

static bool stop_requested(void)
{
    bool requested = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    requested = s_stop_requested || audio_player_cancel_requested();
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return requested;
}

static int take_track_step_requested(void)
{
    int step = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_prev_requested) {
        step = -1;
    } else if (s_next_requested) {
        step = 1;
    }
    s_prev_requested = false;
    s_next_requested = false;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return step;
}

static bool take_track_select_requested(int *index)
{
    bool requested = false;
    int selected = MUSIC_INVALID_INDEX;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    requested = s_select_requested;
    selected = s_select_index;
    s_select_requested = false;
    s_select_index = MUSIC_INVALID_INDEX;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (requested && index) {
        *index = selected;
    }
    return requested;
}

static void clear_stop_requested(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_stop_requested = false;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void mark_playing(bool playing)
{
    bool changed = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    changed = s_playing != playing;
    s_playing = playing;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (changed) {
        notify_state();
    }
}

static esp_err_t set_track_index(int index)
{
    bool changed = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (index < 0 || index >= s_track_count) {
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_INVALID_ARG;
    }
    changed = s_track_index != index;
    s_track_index = index;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (changed) {
        notify_state();
    }
    return ESP_OK;
}

static bool sd_retry_is_deferred(void)
{
    if (!s_sd_fail_latched) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    return (int32_t)(now - s_sd_retry_after_tick) < 0;
}

static void release_sd_periph_set(void)
{
    if (!s_periph_set) {
        return;
    }
    esp_periph_set_stop_all(s_periph_set);
    esp_periph_set_destroy(s_periph_set);
    s_periph_set = NULL;
}

static void sort_tracks(void)
{
    for (int i = 1; i < s_track_count; ++i) {
        char current[MUSIC_PATH_MAX];
        strlcpy(current, s_tracks[i], sizeof(current));
        int j = i - 1;
        while (j >= 0 && strcasecmp(s_tracks[j], current) > 0) {
            strlcpy(s_tracks[j + 1], s_tracks[j], MUSIC_PATH_MAX);
            --j;
        }
        strlcpy(s_tracks[j + 1], current, MUSIC_PATH_MAX);
    }
}

static esp_err_t scan_dir_for_tracks(const char *root)
{
    DIR *dir = opendir(root);
    if (!dir) {
        ESP_LOGW(TAG, "open %s failed", root);
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_track_count < MUSIC_MAX_TRACKS) {
        if (entry->d_name[0] == '.' || !is_prepared_audio_file(entry->d_name)) {
            continue;
        }

        char path[MUSIC_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(path)) {
            ESP_LOGW(TAG, "skip long music path root=%s name=%s", root, entry->d_name);
            continue;
        }

        struct stat st = {0};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        strlcpy(s_tracks[s_track_count], path, MUSIC_PATH_MAX);
        ++s_track_count;
    }
    closedir(dir);
    return s_track_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t mount_sdcard_once(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (sd_retry_is_deferred()) {
        return s_last_sd_error != ESP_OK ? s_last_sd_error : ESP_ERR_TIMEOUT;
    }
    release_sd_periph_set();

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    s_periph_set = esp_periph_set_init(&periph_cfg);
    if (!s_periph_set) {
        ESP_LOGE(TAG, "esp_periph_set_init failed");
        s_last_sd_error = ESP_ERR_NO_MEM;
        s_sd_fail_latched = true;
        s_sd_retry_after_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MUSIC_SD_FAIL_COOLDOWN_MS);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = audio_board_sdcard_init(s_periph_set, SD_MODE_1_LINE);
    if (ret != ESP_OK) {
        ++s_sd_fail_count;
        s_last_sd_error = ret;
        s_sd_fail_latched = true;
        s_sd_retry_after_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MUSIC_SD_FAIL_COOLDOWN_MS);
        ESP_LOGW(TAG,
                 "sdcard mount failed: %s fail_count=%" PRIu32 " retry_after_ms=%d",
                 esp_err_to_name(ret),
                 s_sd_fail_count,
                 MUSIC_SD_FAIL_COOLDOWN_MS);
        release_sd_periph_set();
        return ret;
    }
    s_mounted = true;
    s_sd_fail_latched = false;
    s_last_sd_error = ESP_OK;
    s_sd_fail_count = 0;
    set_status("MUSIC SD OK");
    return ESP_OK;
}

static esp_err_t refresh_playlist(void)
{
    esp_err_t ret = mount_sdcard_once();
    if (ret != ESP_OK) {
        set_status("MUSIC SD FAIL");
        return ret;
    }

    s_track_count = 0;
    ret = scan_dir_for_tracks(MUSIC_ROOT_PRIMARY);
    if (ret != ESP_OK) {
        ret = scan_dir_for_tracks(MUSIC_ROOT_FALLBACK);
    }
    if (s_track_count <= 0) {
        set_status("MUSIC NO WAV");
        ESP_LOGW(TAG, "no prepared wav/pcm found under %s or %s", MUSIC_ROOT_PRIMARY, MUSIC_ROOT_FALLBACK);
        return ESP_ERR_NOT_FOUND;
    }
    sort_tracks();
    if (s_track_index < 0 || s_track_index >= s_track_count) {
        s_track_index = 0;
    }
    char status[MUSIC_PLAYER_STATUS_MAX];
    snprintf(status, sizeof(status), "MUSIC LIST %d", s_track_count);
    set_status(status);
    ESP_LOGI(TAG, "playlist tracks=%d current=%s", s_track_count, s_tracks[s_track_index]);
    for (int i = 0; i < s_track_count; ++i) {
        ESP_LOGI(TAG, "playlist[%02d]=%s", i + 1, s_tracks[i]);
    }
    return ESP_OK;
}

static esp_err_t open_pcm_file(const char *path, music_file_stream_t *stream)
{
    if (!path || !stream) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stream, 0, sizeof(*stream));
    stream->file = fopen(path, "rb");
    if (!stream->file) {
        ESP_LOGW(TAG, "open track failed path=%s", path);
        return ESP_FAIL;
    }

    if (has_extension(path, ".pcm")) {
        stream->bounded = false;
        return ESP_OK;
    }

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), stream->file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "unsupported wav header path=%s", path);
        fclose(stream->file);
        stream->file = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_fmt = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    while (true) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), stream->file) != sizeof(chunk)) {
            break;
        }
        uint32_t chunk_size = read_u32_le(chunk + 4);
        long payload_pos = ftell(stream->file);
        if (payload_pos < 0) {
            break;
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), stream->file) != sizeof(fmt)) {
                break;
            }
            audio_format = read_u16_le(fmt);
            channels = read_u16_le(fmt + 2);
            sample_rate = read_u32_le(fmt + 4);
            bits = read_u16_le(fmt + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt ||
                audio_format != 1 ||
                channels != MUSIC_OUTPUT_CHANNELS ||
                sample_rate != MUSIC_OUTPUT_RATE ||
                bits != MUSIC_OUTPUT_BITS) {
                ESP_LOGW(TAG,
                         "wav must be PCM %dHz %dbit mono path=%s fmt=%u rate=%u bits=%u ch=%u",
                         MUSIC_OUTPUT_RATE,
                         MUSIC_OUTPUT_BITS,
                         path,
                         audio_format,
                         (unsigned)sample_rate,
                         bits,
                         channels);
                break;
            }
            stream->bounded = true;
            stream->data_remaining = chunk_size & ~1U;
            return ESP_OK;
        }

        uint32_t skip = chunk_size + (chunk_size & 1U);
        if (fseek(stream->file, payload_pos + (long)skip, SEEK_SET) != 0) {
            break;
        }
    }

    fclose(stream->file);
    stream->file = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}

static void close_pcm_file(music_file_stream_t *stream)
{
    if (!stream || !stream->file) {
        return;
    }
    fclose(stream->file);
    stream->file = NULL;
}

static int read_pcm_chunk(music_file_stream_t *stream, uint8_t *buffer, size_t buffer_size)
{
    if (!stream || !stream->file || !buffer || buffer_size == 0) {
        return -1;
    }
    size_t want = buffer_size & ~(size_t)1;
    if (stream->bounded) {
        if (stream->data_remaining == 0) {
            return 0;
        }
        if (want > stream->data_remaining) {
            want = stream->data_remaining;
        }
    }
    size_t got = fread(buffer, 1, want, stream->file);
    got &= ~(size_t)1;
    if (stream->bounded) {
        stream->data_remaining -= (uint32_t)got;
    }
    if (got == 0) {
        return 0;
    }
    return (int)got;
}

static esp_err_t play_track(const char *path)
{
    music_file_stream_t stream = {0};
    esp_err_t ret = open_pcm_file(path, &stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "track open/format failed index=%d/%d ret=%s path=%s",
                 s_track_index + 1,
                 s_track_count,
                 esp_err_to_name(ret),
                 path ? path : "");
        return ret;
    }

    bool buffer_from_audio_malloc = false;
    uint8_t *buffer = heap_caps_malloc(MUSIC_STREAM_CHUNK_BYTES, MUSIC_STREAM_BUFFER_CAPS);
    if (!buffer) {
        buffer = audio_malloc(MUSIC_STREAM_CHUNK_BYTES);
        buffer_from_audio_malloc = buffer != NULL;
    }
    if (!buffer) {
        close_pcm_file(&stream);
        return ESP_ERR_NO_MEM;
    }

    char status[MUSIC_PLAYER_STATUS_MAX];
    snprintf(status, sizeof(status), "MUSIC %d/%d", s_track_index + 1, s_track_count);
    set_status(status);
    ESP_LOGI(TAG, "play prepared audio %s", path);

    clear_stop_requested();
    mark_playing(true);
    ret = audio_player_stream_begin("music", true);
    while (ret == ESP_OK && !stop_requested()) {
        int read = read_pcm_chunk(&stream, buffer, MUSIC_STREAM_CHUNK_BYTES);
        if (read > 0) {
            ret = audio_player_stream_write_pcm16(buffer, (size_t)read, "music");
            continue;
        }
        if (read == 0) {
            break;
        }
        ret = ESP_FAIL;
        break;
    }

    if (buffer_from_audio_malloc) {
        audio_free(buffer);
    } else {
        heap_caps_free(buffer);
    }
    audio_player_stream_end("music", 40);
    close_pcm_file(&stream);
    mark_playing(false);
    if (stop_requested()) {
        ESP_LOGI(TAG, "music stopped");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "music track done ret=%s", esp_err_to_name(ret));
    return ret;
}

static void advance_track(void)
{
    if (s_track_count <= 0) {
        s_track_index = 0;
        return;
    }
    s_track_index = (s_track_index + 1) % s_track_count;
}

static void retreat_track(void)
{
    if (s_track_count <= 0) {
        s_track_index = 0;
        return;
    }
    s_track_index = (s_track_index + s_track_count - 1) % s_track_count;
}

static void step_track(int step)
{
    if (step > 0) {
        advance_track();
    } else if (step < 0) {
        retreat_track();
    }
    notify_state();
}

static void drain_pending_music_command(bool *keep_playing)
{
    music_cmd_t cmd = {0};
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd.id == MUSIC_CMD_STOP) {
            request_stop(0);
            *keep_playing = false;
            return;
        }
        if (cmd.id == MUSIC_CMD_NEXT) {
            step_track(1);
            clear_stop_requested();
            *keep_playing = true;
            return;
        }
        if (cmd.id == MUSIC_CMD_PREV) {
            step_track(-1);
            clear_stop_requested();
            *keep_playing = true;
            return;
        }
        if (cmd.id == MUSIC_CMD_PLAY) {
            clear_stop_requested();
            *keep_playing = true;
        }
    }
}

static void music_task(void *arg)
{
    (void)arg;
    music_cmd_t cmd = {0};
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd.id == MUSIC_CMD_STOP) {
            request_stop(0);
            set_status("MUSIC STOP");
            continue;
        }
        if (cmd.id == MUSIC_CMD_NEXT) {
            step_track(1);
        }
        if (cmd.id == MUSIC_CMD_PREV) {
            step_track(-1);
        }
        if (cmd.id == MUSIC_CMD_REFRESH) {
            refresh_playlist();
            continue;
        }
        if (cmd.id == MUSIC_CMD_SELECT) {
            if (cmd.index < 0) {
                set_status("MUSIC BAD SEL");
                continue;
            }
            set_track_index(cmd.index);
            clear_stop_requested();
        }

        esp_err_t ret = refresh_playlist();
        if (ret != ESP_OK) {
            continue;
        }

        bool keep_playing = true;
        while (keep_playing && s_track_count > 0) {
            ret = play_track(s_tracks[s_track_index]);
            int selected_index = MUSIC_INVALID_INDEX;
            bool select_requested = take_track_select_requested(&selected_index);
            int step_requested = take_track_step_requested();
            if (select_requested) {
                if (set_track_index(selected_index) == ESP_OK) {
                    clear_stop_requested();
                    keep_playing = true;
                } else {
                    set_status("MUSIC BAD SEL");
                    keep_playing = false;
                    clear_stop_requested();
                }
            } else if (step_requested != 0) {
                step_track(step_requested);
                clear_stop_requested();
                keep_playing = true;
            } else if (ret == ESP_ERR_INVALID_STATE) {
                keep_playing = false;
                clear_stop_requested();
            } else if (ret != ESP_OK) {
                char status[MUSIC_PLAYER_STATUS_MAX];
                snprintf(status, sizeof(status), "MUSIC ERR %02d", s_track_index + 1);
                set_status(status);
                clear_stop_requested();
                keep_playing = false;
            } else {
                advance_track();
                notify_state();
                keep_playing = true;
            }
            drain_pending_music_command(&keep_playing);
        }
        if (!keep_playing) {
            set_status("MUSIC IDLE");
        }
    }
}

static esp_err_t queue_music_cmd_arg(music_cmd_id_t id, int index)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    music_cmd_t cmd = {.id = id, .index = index};
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "music command queue full id=%d", id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t queue_music_cmd(music_cmd_id_t id)
{
    return queue_music_cmd_arg(id, MUSIC_INVALID_INDEX);
}

esp_err_t music_player_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreateWithCaps(MUSIC_CMD_QUEUE_LEN, sizeof(music_cmd_t), MUSIC_QUEUE_CAPS);
        if (!s_cmd_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(music_task,
                                                   "music_player",
                                                   MUSIC_TASK_STACK,
                                                   NULL,
                                                   3,
                                                   NULL,
                                                   0,
                                                   MUSIC_TASK_STACK_CAPS);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "music task create failed");
        return ESP_ERR_NO_MEM;
    }
    set_status("MUSIC READY");
    queue_music_cmd(MUSIC_CMD_REFRESH);
    return ESP_OK;
}

esp_err_t music_player_play(void)
{
    clear_stop_requested();
    return queue_music_cmd(MUSIC_CMD_PLAY);
}

esp_err_t music_player_stop(void)
{
    request_stop(0);
    return queue_music_cmd(MUSIC_CMD_STOP);
}

esp_err_t music_player_next(void)
{
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    was_playing = request_stop_locked(1);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (was_playing) {
        audio_player_cancel();
        return ESP_OK;
    }
    return queue_music_cmd(MUSIC_CMD_NEXT);
}

esp_err_t music_player_prev(void)
{
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    was_playing = request_stop_locked(-1);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (was_playing) {
        audio_player_cancel();
        return ESP_OK;
    }
    return queue_music_cmd(MUSIC_CMD_PREV);
}

esp_err_t music_player_toggle(void)
{
    return music_player_is_playing() ? music_player_stop() : music_player_play();
}

esp_err_t music_player_refresh(void)
{
    return queue_music_cmd(MUSIC_CMD_REFRESH);
}

esp_err_t music_player_select_visible(int row)
{
    if (row < 0 || row >= MUSIC_PLAYER_LIST_LINES) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = MUSIC_INVALID_INDEX;
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_track_count > 0) {
        int start = visible_start_for_index(s_track_index, s_track_count);
        index = start + row;
        if (index >= s_track_count) {
            index = MUSIC_INVALID_INDEX;
        }
    }
    if (index >= 0) {
        was_playing = s_playing;
        if (was_playing) {
            request_select_locked(index);
        } else {
            s_select_requested = false;
            s_select_index = MUSIC_INVALID_INDEX;
            s_stop_requested = false;
            s_next_requested = false;
            s_prev_requested = false;
            s_track_index = index;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    if (index < 0) {
        set_status("MUSIC BAD SEL");
        return ESP_ERR_INVALID_ARG;
    }

    notify_state();
    if (was_playing) {
        audio_player_cancel();
        return ESP_OK;
    }
    return queue_music_cmd_arg(MUSIC_CMD_SELECT, index);
}

void music_player_set_state_callback(music_player_state_cb_t cb, void *ctx)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_state_cb = cb;
    s_state_ctx = ctx;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    notify_state();
}
