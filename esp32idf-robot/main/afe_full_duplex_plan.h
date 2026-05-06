#pragma once

/*
 * Official full-duplex/AEC migration notes for ESP32-S3-Korvo-2.
 *
 * Keep the current mono PCM capture path enabled by default.  The board can do
 * full duplex, but the software path must be switched as a group:
 *
 * 1. Add a "model" partition and enable the needed esp-sr models.
 * 2. Add audio_recorder and esp-sr to the main component requirements.
 * 3. Replace mic_diag's mono I2S reader with the ESP-SR AFE feed/fetch path.
 * 4. Feed ES7210 interleaved mic/reference samples into AFE.
 * 5. Use AFE VAD_START/VAD_END for continuous chat end detection.
 * 6. Only after AEC is verified, allow recording while assistant playback is busy.
 *
 * Official references in the local ADF checkout:
 * - examples/advanced_examples/aec/main/aec_examples.c
 *   Korvo-2 single-mic AEC uses input format "RM"; dual-mic AEC uses "RMNM".
 * - examples/speech_recognition/wwe/main/main.c
 *   Uses recorder_sr + audio_recorder events for WakeNet/VAD.
 * - examples/ai_agent/volc_rtc/components/audio_processor/
 *   Uses recorder_sr with AEC enabled for real-time voice communication.
 *
 * The current firmware still records and plays 16000 Hz / 16-bit / mono.
 */

#define ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL 1
