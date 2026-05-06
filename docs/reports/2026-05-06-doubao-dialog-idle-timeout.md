# 2026-05-06 Doubao Dialog Idle Timeout Report

## Summary

ESP32 voice requests were already reaching the Doubao Realtime Dialog service with valid credentials and a valid Dialog resource. The reported failure was not caused by a wrong App ID, App Key, Access Token, or instance resource ID.

The failure was caused by short file-style ESP32 uploads ending without enough trailing silence for Doubao ASR endpointing. The service recognized the beginning of the utterance, then waited for more audio and eventually returned `ClientLackDataError` followed by `DialogAudioIdleTimeoutError`.

## Evidence

- Current Dialog config had non-empty `DOUBAO_DIALOG_APP_ID`, `DOUBAO_DIALOG_APP_KEY`, and `DOUBAO_DIALOG_ACCESS_TOKEN`.
- `DOUBAO_DIALOG_RESOURCE_ID` was not set by environment, so the MCP default `volc.speech.dialog` was used.
- A historical known-good WAV (`esp32-14095-1.wav`) completed the full path: ASR, chat response, and TTS audio.
- The failing short WAV from the user log (`esp32-10393105-2.wav`) was recognized as a short greeting before failing.
- The failing run showed `pcm=45056 tail=14400 chunk=1280`; at 16 kHz, 16-bit mono, `14400` bytes is only 450 ms of trailing silence.
- Doubao ASR response metadata reported `eos_silence_timeout=1500`, so a 450 ms tail is too short for reliable short-utterance endpointing.

## Fix

Updated MCP Dialog upload defaults in `MCP-robot/app_config.py`:

- `DOUBAO_DIALOG_VAD_TAIL_SILENCE_MS`: default changed to `1600`
- Added an inline comment explaining the relationship with Doubao ASR `eos_silence_timeout=1500`
- Kept ESP32 playback source format unchanged: 16 kHz, 16-bit, mono
- Kept ESP32 voice routing on Doubao Realtime Dialog; no MiMo fallback was added for ESP32 voice

## Validation

Direct MCP-side test with the same short WAV passed after the fix:

- Received `ASR_ENDED`
- Received TTS audio frames
- Received `CHAT_ENDED`
- Received `TTS_ENDED`

The MCP service was restarted after the change, and the ESP32 reconnected successfully.

## Notes

- The published firmware keeps `esp32idf-robot/main/app_config.h` sanitized with placeholder Wi-Fi and MCP URI values.
- Runtime data, audio captures, logs, virtual environments, build output, and managed ESP-IDF components were not added to the repository.
