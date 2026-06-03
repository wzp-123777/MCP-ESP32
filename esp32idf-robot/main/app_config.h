#pragma once

#define ROBOT_WIFI_SSID "YOUR_WIFI_SSID"
#define ROBOT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define ROBOT_MCP_URI "ws://YOUR_MCP_SERVER_IP:8080/esp32_ws"
#define ROBOT_DEVICE_ID "ESP32_KORVO_2"

#define ROBOT_AUDIO_SAMPLE_RATE 16000
#define ROBOT_AUDIO_BITS 16
#define ROBOT_AUDIO_CHANNELS 1

/*
 * Optional demo hardware.
 * Defaults keep the public build safe without external wiring. The firmware
 * still accepts QQ/console commands and reports stub telemetry.
 */
#define ROBOT_ROOM_LIGHT_GPIO -1
#define ROBOT_ROOM_LIGHT_ACTIVE_LOW 0
#define ROBOT_CAMERA_REAL_CAPTURE 0
