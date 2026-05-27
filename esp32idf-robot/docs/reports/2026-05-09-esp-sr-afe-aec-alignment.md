# 2026-05-09 ESP-SR AFE/AEC Alignment Report

## Scope

This report covers the Korvo-2 firmware AFE/AEC work done after the raw TDM channel diagnosis. It focuses on whether the official ESP-SR feed/fetch path is wired correctly and whether board playback is being suppressed enough for future full-duplex/barge-in work.

No GitHub push was performed.

## Firmware Changes

Changed file:

- `D:\esp32\esp32idf-robot\main\afe_capture.c`

Changes made:

- Kept official Korvo-2 raw capture format: `RMNM`.
- Kept capture bus at `16000 Hz`, `32 bit`, `I2S_CHANNEL_FMT_RIGHT_LEFT`.
- Kept playback logical source unchanged: `16000 Hz / 16 bit / mono`, default volume `75`.
- Switched AFE mode from `AFE_MODE_LOW_COST` to `AFE_MODE_HIGH_PERF`.
- Switched AEC mode from `AEC_MODE_SR_LOW_COST` to `AEC_MODE_SR_HIGH_PERF`.
- Moved AFE task placement closer to official examples:
  - `afe_feed` on core 1.
  - `afe_fetch` on core 0.
- Adjusted ES7210 gain:
  - MIC3: `GAIN_30DB`.
  - MIC1/MIC2: `GAIN_33DB`.

## Confirmed Architecture

The AFE config printout confirms:

- `pcm_config.total_ch_num: 4`
- `pcm_config.mic_num: 2: [ ch1, ch3 ]`
- `pcm_config.ref_num: 1: [ ch0 ]`
- `pcm_config.sample_rate: 16000`
- `afe_mode: HIGH PERF`
- `aec mode: SR_HIGH_PERF`
- `aec_filter_length: 4`

Channel meaning is now confirmed:

- `ch0`: playback reference.
- `ch1`: microphone.
- `ch2`: unused/silent channel.
- `ch3`: microphone.
- `afe`: ESP-SR processed output after AEC/VAD pipeline.

## Verification

Build:

- Firmware build passed.
- Binary size: `0x2799c0`.
- Smallest app partition: `0x300000`.
- Free space: `0x86640`, about 17%.

Flash:

- Flashed successfully to `COM3`.
- Device reconnected to the configured MCP WebSocket endpoint.
- `sr_enabled` became `true` after AFE lazy init.

Raw TDM diagnostic:

- Command: `raw_tdm_diag 5000`.
- Playback command repeated during capture: `play`.
- Latest diagnostic session: `esp32-rawtdm-51935-*`.
- Files saved under `D:\esp32\MCP-robot\data\esp32_audio`.

## AEC Results

Latest session after MIC3 gain adjustment:

| Window | ch1 RMS | ch3 RMS | AFE RMS | AFE vs ch1 | AFE vs ch3 |
|---|---:|---:|---:|---:|---:|
| active playback | 3957.6 | 2614.8 | 679.1 | -15.3 dB | -11.7 dB |
| active playback, first 500 ms skipped | 4011.9 | 2633.0 | 614.2 | -16.3 dB | -12.6 dB |
| inactive | 242.4 | 167.4 | 299.3 | +1.8 dB | +5.0 dB |

Compared with earlier tests:

| Session | Config | AFE vs ch1 active | AFE vs ch3 active | Clipping |
|---|---|---:|---:|---|
| `711268` | LOW_COST baseline | -2.9 dB | -8.1 dB | none |
| `78444` | HIGH_PERF | -5.5 dB | -10.0 dB | none |
| `8874` | HIGH_PERF + core split | -4.9 dB | -12.1 dB | ch3 clipped |
| `51935` | HIGH_PERF + core split + MIC3 30 dB | -15.3 dB | -11.7 dB | none |

The best practical improvement came from reducing MIC3 gain. Before that, ch3 could clip at `32768`, which makes echo cancellation unreliable because the echo path becomes nonlinear.

## Remaining Problems

This is not yet true full duplex.

Main blockers:

- AEC is now useful, but not strong enough to trust blindly during real assistant speech plus human interruption.
- The latest test used repeated onboard `xiaole` playback, not real Doubao TTS while a human talks over it.
- Current continuous chat still has a sequential state machine bias: record, upload, wait, play. True full duplex needs simultaneous capture, upload, playback, and interruption handling.
- AFE VAD still triggers sometimes after playback or in quiet periods. That means VAD gating still needs a second-stage policy around assistant playback and barge-in.
- AEC suppression depends on reference alignment and speaker path stability. Any volume, speaker position, enclosure change, or clipping can reduce cancellation.
- Firmware resource pressure is still real. Core split removed the observed upload-stage watchdog in the monitored test, but AFE + Wi-Fi + UI + playback remain close enough that more long-duration stress tests are needed.

## Next Steps

Recommended next engineering steps:

1. Add a repeatable long playback diagnostic using actual MCP/Doubao TTS PCM, not only `xiaole`.
2. Add a barge-in diagnostic mode:
   - assistant playback active,
   - user speaks during playback,
   - save raw channels and AFE output.
3. Tune VAD policy after AEC:
   - require stronger speech evidence during assistant playback,
   - suppress short false VAD bursts,
   - keep real barge-in possible.
4. Stress test for 10 to 30 minutes:
   - continuous AFE running,
   - repeated playback,
   - MCP upload,
   - watch WDT, heap, reconnects, and `sr_enabled`.
5. Only after the above should the chat UX move toward true streaming full duplex.

## Current Conclusion

The official ESP-SR AFE/AEC path is now structurally aligned and measurably suppressing board playback. The latest measured suppression is about `-15 dB` versus ch1 and `-12 dB` versus ch3 during playback-active windows, with no clipping.

This is enough to continue toward barge-in experiments. It is not enough yet to claim reliable true full duplex.
