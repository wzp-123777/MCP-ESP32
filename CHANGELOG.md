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

## v2.0

Espressif-aligned full-duplex AEC candidate.

- Adds an explicit firmware dependency on Espressif ESP-SR 2.4.4 instead of relying on the ADF-vendored ESP-SR 2.1.5 component.
- Switches AFE creation to `AFE_TYPE_FD` for the official full-duplex scenario.
- Uses `AEC_MODE_FD_LOW_COST` first to keep ESP32-S3 CPU load conservative.
- Enables aggressive AEC NLP and keeps noise suppression active when a playback reference channel is present.
- Keeps the v1.5 asynchronous upload queue and playback-aware barge-in gate as fallback protection while hardware tuning continues.
- The `v2.0-espressif-fd-aec` branch also keeps ESP32-S3 defaults aligned with the development firmware by enabling the `wn9_hiesp` WakeNet model and conservative PSRAM/internal allocation defaults.

Validation still required:

- Flash on COM3 and confirm logs show the managed ESP-SR 2.4.4 component.
- Re-run `BARGE` and continuous-chat interruption tests.
- Compare `ref_corr`, `ref_lag_ms`, self-echo VAD behavior, and whether real near-end speech remains accepted.
