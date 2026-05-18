#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t mcp_client_init(void);
esp_err_t mcp_client_set_endpoint(const char *endpoint);
esp_err_t mcp_client_reset_endpoint_to_default(void);
esp_err_t mcp_client_connect(void);
void mcp_client_disconnect(void);
const char *mcp_client_get_endpoint(void);
const char *mcp_client_get_status_text(void);
bool mcp_client_is_configured(void);
bool mcp_client_is_wifi_connected(void);
bool mcp_client_is_connected(void);
bool mcp_client_is_assistant_busy(void);
void mcp_client_cancel_playback(void);
void mcp_client_set_sr_enabled(bool enabled);
typedef void (*mcp_client_busy_cb_t)(bool busy, void *ctx);
void mcp_client_set_busy_callback(mcp_client_busy_cb_t cb, void *ctx);
typedef void (*mcp_client_playback_cb_t)(bool busy, void *ctx);
void mcp_client_set_playback_callback(mcp_client_playback_cb_t cb, void *ctx);
typedef void (*mcp_client_device_command_cb_t)(const char *command, void *ctx);
void mcp_client_set_device_command_callback(mcp_client_device_command_cb_t cb, void *ctx);
esp_err_t mcp_client_send_text_request(const char *text);
esp_err_t mcp_client_send_diagnostic_event(const char *name, const char *phase, const char *detail);
esp_err_t mcp_client_audio_stream_begin(const char *session_id);
esp_err_t mcp_client_audio_stream_begin_with_source(const char *session_id, const char *source);
esp_err_t mcp_client_audio_stream_chunk(const char *session_id, const uint8_t *data, size_t len);
esp_err_t mcp_client_audio_stream_end(const char *session_id, uint32_t duration_ms, const char *reason);
esp_err_t mcp_client_send_config(const char *persona_id,
                                 const char *persona_label,
                                 const char *voice_id,
                                 const char *voice_label,
                                 bool continuous_chat,
                                 bool wake_enabled);
esp_err_t mcp_client_send_telemetry(void);
