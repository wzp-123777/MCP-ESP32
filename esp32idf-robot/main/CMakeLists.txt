idf_component_register(
    SRCS "robot_voice_main.c" "audio_player.c" "music_player.c" "robot_peripherals.c" "mic_diag.c" "afe_capture.c" "app_ui.c" "app_ui_font_zh_16.c" "mcp_client.c" "app_buttons.c"
    INCLUDE_DIRS "."
    REQUIRES audio_board audio_hal audio_pipeline audio_stream audio_recorder esp-adf-libs esp-sr esp_peripherals esp_lcd input_key_service esp_wifi esp_netif esp_event nvs_flash esp_websocket_client mbedtls lvgl
    EMBED_FILES "xiaole_16k_stereo.pcm" "barge_after_prompt_16k_mono.wav" "barge_interrupt_prompt_16k_mono.wav"
)
