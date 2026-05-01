idf_component_register(
    SRCS "robot_voice_main.c" "audio_player.c" "mic_diag.c" "app_ui.c" "mcp_client.c" "app_buttons.c"
    INCLUDE_DIRS "."
    REQUIRES audio_board audio_hal audio_pipeline audio_stream esp_peripherals esp_lcd input_key_service esp_wifi esp_netif esp_event nvs_flash esp_websocket_client mbedtls lvgl
    EMBED_FILES "xiaole_16k_stereo.pcm"
)
