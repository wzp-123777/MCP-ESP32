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

#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_pipeline.h"
#include "audio_player.h"
#include "board.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "fatfs_stream.h"
#include "filter_resample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3_decoder.h"
#include "raw_stream.h"
#include "esp_heap_caps.h"

#define MUSIC_ROOT_PRIMARY "/sdcard/music"
#define MUSIC_ROOT_FALLBACK "/sdcard"
#define MUSIC_MAX_TRACKS 48
#define MUSIC_PATH_MAX 192
#define MUSIC_CMD_QUEUE_LEN 6
#define MUSIC_TASK_STACK 6144
#define MUSIC_DECODE_CHUNK_BYTES 2048
#define MUSIC_OUTPUT_RATE 16000
#define MUSIC_OUTPUT_CHANNELS 1
#define MUSIC_OUTPUT_BITS 16
#define MUSIC_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSIC_QUEUE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MUSIC_SD_FAIL_COOLDOWN_MS 30000

typedef enum {
    MUSIC_CMD_PLAY = 0,
    MUSIC_CMD_STOP,
    MUSIC_CMD_NEXT,
} music_cmd_id_t;

typedef struct {
    music_cmd_id_t id;
} music_cmd_t;

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t fatfs;
    audio_element_handle_t mp3;
    audio_element_handle_t filter;
    audio_element_handle_t raw;
    audio_event_iface_handle_t evt;
} music_pipeline_t;

static const char *TAG = "MUSIC_PLAYER";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmd_queue;
static esp_periph_set_handle_t s_periph_set;
static audio_pipeline_handle_t s_active_pipeline;
static bool s_mounted;
static bool s_playing;
static bool s_stop_requested;
static bool s_next_requested;
static bool s_sd_fail_latched;
static TickType_t s_sd_retry_after_tick;
static esp_err_t s_last_sd_error = ESP_OK;
static uint32_t s_sd_fail_count;
static char s_tracks[MUSIC_MAX_TRACKS][MUSIC_PATH_MAX];
static int s_track_count;
static int s_track_index;
static char s_status[80] = "MUSIC IDLE";

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

static bool request_stop_locked(bool next)
{
    bool was_playing = s_playing;
    s_stop_requested = true;
    if (next) {
        s_next_requested = true;
    }
    return was_playing;
}

static void request_stop(bool next)
{
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    was_playing = request_stop_locked(next);
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

static bool take_next_requested(void)
{
    bool requested = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    requested = s_next_requested;
    s_next_requested = false;
    if (s_lock) {
        xSemaphoreGive(s_lock);
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
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_playing = playing;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void set_active_pipeline(audio_pipeline_handle_t pipeline)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_active_pipeline = pipeline;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool has_mp3_extension(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && strcasecmp(dot, ".mp3") == 0;
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

static esp_err_t scan_dir_for_mp3(const char *root)
{
    DIR *dir = opendir(root);
    if (!dir) {
        ESP_LOGW(TAG, "open %s failed", root);
        return ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_track_count < MUSIC_MAX_TRACKS) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!has_mp3_extension(entry->d_name)) {
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
    ret = scan_dir_for_mp3(MUSIC_ROOT_PRIMARY);
    if (ret != ESP_OK) {
        ret = scan_dir_for_mp3(MUSIC_ROOT_FALLBACK);
    }
    if (s_track_count <= 0) {
        set_status("MUSIC NO MP3");
        ESP_LOGW(TAG, "no mp3 found under %s or %s", MUSIC_ROOT_PRIMARY, MUSIC_ROOT_FALLBACK);
        return ESP_ERR_NOT_FOUND;
    }
    sort_tracks();
    if (s_track_index < 0 || s_track_index >= s_track_count) {
        s_track_index = 0;
    }
    ESP_LOGI(TAG, "playlist tracks=%d current=%s", s_track_count, s_tracks[s_track_index]);
    return ESP_OK;
}

static bool pipeline_stop_event(const audio_event_iface_msg_t *msg, const music_pipeline_t *pipe)
{
    if (!msg || !pipe || msg->source_type != AUDIO_ELEMENT_TYPE_ELEMENT) {
        return false;
    }
    if (msg->cmd != AEL_MSG_CMD_REPORT_STATUS) {
        return false;
    }
    audio_element_state_t state = audio_element_get_state((audio_element_handle_t)msg->source);
    if (state == AEL_STATE_FINISHED || state == AEL_STATE_STOPPED || state == AEL_STATE_ERROR) {
        ESP_LOGI(TAG, "pipeline element stopped state=%d source=%p", state, msg->source);
        return msg->source == pipe->fatfs || msg->source == pipe->mp3 || msg->source == pipe->filter ||
               msg->source == pipe->raw;
    }
    return false;
}

static void handle_pipeline_events(music_pipeline_t *pipe, bool *pipeline_finished)
{
    audio_event_iface_msg_t msg = {0};
    while (audio_event_iface_listen(pipe->evt, &msg, 0) == ESP_OK) {
        if (msg.source == (void *)pipe->mp3 && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t info = {0};
            audio_element_getinfo(pipe->mp3, &info);
            ESP_LOGI(TAG,
                     "mp3 info rate=%d bits=%d ch=%d",
                     info.sample_rates,
                     info.bits,
                     info.channels);
            if (info.sample_rates > 0 && info.channels > 0) {
                rsp_filter_change_src_info(pipe->filter, info.sample_rates, info.channels, info.bits > 0 ? info.bits : 16);
            }
            continue;
        }
        if (pipeline_stop_event(&msg, pipe)) {
            *pipeline_finished = true;
        }
    }
}

static void deinit_pipeline(music_pipeline_t *pipe)
{
    if (!pipe || !pipe->pipeline) {
        return;
    }
    audio_pipeline_stop(pipe->pipeline);
    audio_pipeline_wait_for_stop(pipe->pipeline);
    audio_pipeline_terminate(pipe->pipeline);
    if (pipe->evt) {
        audio_pipeline_remove_listener(pipe->pipeline);
        audio_event_iface_destroy(pipe->evt);
        pipe->evt = NULL;
    }
    if (pipe->fatfs) {
        audio_pipeline_unregister(pipe->pipeline, pipe->fatfs);
    }
    if (pipe->mp3) {
        audio_pipeline_unregister(pipe->pipeline, pipe->mp3);
    }
    if (pipe->filter) {
        audio_pipeline_unregister(pipe->pipeline, pipe->filter);
    }
    if (pipe->raw) {
        audio_pipeline_unregister(pipe->pipeline, pipe->raw);
    }
    audio_pipeline_deinit(pipe->pipeline);
    if (pipe->fatfs) {
        audio_element_deinit(pipe->fatfs);
    }
    if (pipe->mp3) {
        audio_element_deinit(pipe->mp3);
    }
    if (pipe->filter) {
        audio_element_deinit(pipe->filter);
    }
    if (pipe->raw) {
        audio_element_deinit(pipe->raw);
    }
    memset(pipe, 0, sizeof(*pipe));
}

static esp_err_t init_pipeline(const char *path, music_pipeline_t *pipe)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipe->pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!pipe->pipeline) {
        return ESP_ERR_NO_MEM;
    }

    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    pipe->fatfs = fatfs_stream_init(&fatfs_cfg);

    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.task_prio = 5;
    mp3_cfg.task_core = 0;
    mp3_cfg.stack_in_ext = true;
    pipe->mp3 = mp3_decoder_init(&mp3_cfg);

    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.dest_rate = MUSIC_OUTPUT_RATE;
    rsp_cfg.dest_ch = MUSIC_OUTPUT_CHANNELS;
    rsp_cfg.dest_bits = MUSIC_OUTPUT_BITS;
    rsp_cfg.src_rate = 44100;
    rsp_cfg.src_ch = 2;
    rsp_cfg.src_bits = 16;
    rsp_cfg.max_indata_bytes = 1024;
    rsp_cfg.out_len_bytes = 1024;
    rsp_cfg.task_prio = 5;
    rsp_cfg.task_core = 0;
    rsp_cfg.stack_in_ext = true;
    pipe->filter = rsp_filter_init(&rsp_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 8 * 1024;
    pipe->raw = raw_stream_init(&raw_cfg);

    if (!pipe->fatfs || !pipe->mp3 || !pipe->filter || !pipe->raw) {
        deinit_pipeline(pipe);
        return ESP_ERR_NO_MEM;
    }

    audio_element_set_uri(pipe->fatfs, path);
    audio_element_set_input_timeout(pipe->raw, pdMS_TO_TICKS(100));

    audio_pipeline_register(pipe->pipeline, pipe->fatfs, "file");
    audio_pipeline_register(pipe->pipeline, pipe->mp3, "mp3");
    audio_pipeline_register(pipe->pipeline, pipe->filter, "filter");
    audio_pipeline_register(pipe->pipeline, pipe->raw, "raw");
    const char *link_tag[4] = {"file", "mp3", "filter", "raw"};
    esp_err_t ret = audio_pipeline_link(pipe->pipeline, link_tag, 4);
    if (ret != ESP_OK) {
        deinit_pipeline(pipe);
        return ret;
    }

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    pipe->evt = audio_event_iface_init(&evt_cfg);
    if (!pipe->evt) {
        deinit_pipeline(pipe);
        return ESP_ERR_NO_MEM;
    }
    audio_pipeline_set_listener(pipe->pipeline, pipe->evt);
    return ESP_OK;
}

static esp_err_t play_track(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    music_pipeline_t pipe = {0};
    esp_err_t ret = init_pipeline(path, &pipe);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "music pipeline init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    char status[80];
    snprintf(status, sizeof(status), "MUSIC %d/%d", s_track_index + 1, s_track_count);
    set_status(status);
    ESP_LOGI(TAG, "play %s", path);

    clear_stop_requested();
    mark_playing(true);
    set_active_pipeline(pipe.pipeline);
    ret = audio_player_stream_begin("music", true);
    if (ret == ESP_OK) {
        ret = audio_pipeline_run(pipe.pipeline);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "music start failed: %s", esp_err_to_name(ret));
        set_active_pipeline(NULL);
        mark_playing(false);
        deinit_pipeline(&pipe);
        return ret;
    }

    char *buffer = audio_malloc(MUSIC_DECODE_CHUNK_BYTES);
    if (!buffer) {
        request_stop(false);
        ret = ESP_ERR_NO_MEM;
    }

    bool pipeline_finished = false;
    while (ret == ESP_OK && !stop_requested()) {
        handle_pipeline_events(&pipe, &pipeline_finished);
        int read = raw_stream_read(pipe.raw, buffer, MUSIC_DECODE_CHUNK_BYTES);
        if (read > 0) {
            ret = audio_player_stream_write_pcm16((const uint8_t *)buffer, (size_t)read, "music");
            continue;
        }
        if (read == AEL_IO_TIMEOUT) {
            if (pipeline_finished) {
                break;
            }
            continue;
        }
        if (read == AEL_IO_DONE || read == AEL_IO_OK) {
            pipeline_finished = true;
            continue;
        }
        if (pipeline_finished) {
            break;
        }
        ESP_LOGW(TAG, "raw read stopped ret=%d", read);
        ret = ESP_FAIL;
        break;
    }

    if (buffer) {
        audio_free(buffer);
    }
    audio_player_stream_end("music", 80);
    set_active_pipeline(NULL);
    mark_playing(false);
    deinit_pipeline(&pipe);
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

static void drain_pending_music_command(bool *keep_playing)
{
    music_cmd_t cmd = {0};
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd.id == MUSIC_CMD_STOP) {
            request_stop(false);
            *keep_playing = false;
            return;
        }
        if (cmd.id == MUSIC_CMD_NEXT) {
            advance_track();
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
            request_stop(false);
            set_status("MUSIC STOP");
            continue;
        }
        if (cmd.id == MUSIC_CMD_NEXT) {
            advance_track();
        }

        esp_err_t ret = refresh_playlist();
        if (ret != ESP_OK) {
            continue;
        }

        bool keep_playing = true;
        while (keep_playing && s_track_count > 0) {
            ret = play_track(s_tracks[s_track_index]);
            bool next_requested = take_next_requested();
            if (next_requested) {
                advance_track();
                clear_stop_requested();
                keep_playing = true;
            } else if (ret == ESP_ERR_INVALID_STATE) {
                keep_playing = false;
                clear_stop_requested();
            } else {
                advance_track();
                keep_playing = true;
            }
            drain_pending_music_command(&keep_playing);
        }
        if (!keep_playing) {
            set_status("MUSIC IDLE");
        }
    }
}

static esp_err_t queue_music_cmd(music_cmd_id_t id)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    music_cmd_t cmd = {.id = id};
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "music command queue full id=%d", id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
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
                                                   4,
                                                   NULL,
                                                   0,
                                                   MUSIC_TASK_STACK_CAPS);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "music task create failed");
        return ESP_ERR_NO_MEM;
    }
    set_status("MUSIC READY");
    return ESP_OK;
}

esp_err_t music_player_play(void)
{
    clear_stop_requested();
    return queue_music_cmd(MUSIC_CMD_PLAY);
}

esp_err_t music_player_stop(void)
{
    request_stop(false);
    return queue_music_cmd(MUSIC_CMD_STOP);
}

esp_err_t music_player_next(void)
{
    bool was_playing = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    was_playing = request_stop_locked(true);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (was_playing) {
        audio_player_cancel();
        return ESP_OK;
    }
    return queue_music_cmd(MUSIC_CMD_NEXT);
}
