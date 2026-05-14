# MCP ESP32 Robot Versions

This repository contains both parts of the robot workspace:

- `esp32idf-robot`: ESP32-S3-Korvo-2 firmware.
- `MCP-robot`: MCP server and model routing service.

## v1.0

Baseline GitHub version before the latest full-duplex stability work.

- ESP32 voice path uses Doubao Realtime Dialog through MCP.
- QQ/NapCat text path remains routed separately through MiMo.
- ESP32 playback source format remains 16000 Hz, 16-bit, mono.
- Publish-safe firmware config keeps Wi-Fi and MCP endpoint placeholders.

## v1.5

Current tested version with practical barge-in and queue stability improvements.

- Migrates ESP32 capture toward the ESP-SR AFE feed/fetch architecture.
- Adds AFE/AEC diagnostics for channel ordering, playback reference, VAD, and barge-in behavior.
- Adds playback-aware VAD gating so playback self-echo is treated as a candidate before recording starts.
- Cancels local TTS playback when a real barge-in is accepted.
- Moves audio upload from the AFE callback path to an asynchronous upload queue.
- Moves MCP helper tasks and queues to PSRAM where possible to reduce internal heap pressure.
- Persists MCP WebSocket endpoint in NVS and allows runtime update through the serial `MCP URL` command.
- Keeps server-side ESP32 voice traffic on Doubao Realtime Dialog.

Known limitation:

- The current firmware still uses the ADF-vendored ESP-SR 2.1.5 stack, so playback self-echo can still trigger AFE VAD. The v1.5 gate makes barge-in usable, but it is not the official full-duplex AEC solution.

## v2.0 Target

Planned Espressif-aligned full-duplex release.

- Override or upgrade the firmware ESP-SR dependency to Espressif ESP-SR 2.4.3 or newer.
- Use the official ESP32-S3 Full-Duplex AEC/AFE path.
- Evaluate `AEC_MODE_FD_LOW_COST` first, then `AEC_MODE_FD_HIGH_PERF` if needed.
- Validate playback reference stability, reference lag, correlation, and near-end speech preservation.
- Keep the v1.5 asynchronous upload queue and barge-in gate as fallback protection.
