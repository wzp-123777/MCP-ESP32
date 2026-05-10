# ESP32 连续聊天 AFE/AEC VAD 优化报告

日期：2026-05-06

## 背景

本轮继续优化 ESP32-S3-Korvo-2 的“连续聊天”体验。前置约束保持不变：

- ESP32 语音聊天链路继续走豆包 Realtime Dialog。
- NapCat/QQ 文本链路继续走 MiMo。
- ESP32 下行播放逻辑源保持 `16000Hz / 16bit / mono`，默认音量 `75`。
- 发布仓库的 `main/app_config.h` 保持占位符，不写入真实 Wi-Fi、IP、密钥。

上一轮已把豆包短句 `DialogAudioIdleTimeoutError` 通过尾部静音修复，并把 ESP32 连续聊天切到 AFE/AEC 懒加载路径。本轮主要问题变成：连续聊天打开后，环境声、房间人声、播放尾音/AEC 残留会让 VAD 误启动，导致机器人自问自答。

## 本轮修改

修改文件：

- `esp32idf-robot/main/robot_voice_main.c`

主要改动：

1. AEC 连续聊天启动阈值收紧。
   - `CONT_AEC_VAD_START_AVG`: `260 -> 760`
   - `CONT_AEC_VAD_START_PEAK`: `2600 -> 4500`
   - `CONT_AEC_VAD_STOP_AVG`: `70 -> 140`
   - `CONT_AEC_VAD_START_MARGIN`: `140 -> 420`
   - `CONT_AEC_VAD_STOP_MARGIN`: `35 -> 90`
   - `CONT_AEC_VAD_NOISE_FLOOR_INIT`: `24 -> 120`
   - `CONT_AEC_VAD_NOISE_FLOOR_MAX`: `160 -> 300`

2. AEC 路径启动方式从“慢慢攒分”改成“连续强证据”。
   - 旧逻辑：命中一次加 1，未命中只减 1，远场持续人声也容易攒够启动分。
   - 新逻辑：AEC 路径必须同时满足平均能量和峰值门槛；任意一帧不满足就清空启动计数。
   - 非 AEC/普通 PCM 路径保持原来的宽松逻辑，避免影响 PTT 或旧路径。

3. 播放结束后重新监听延迟增加。
   - `CONT_REARM_DELAY_MS`: `1800ms -> 3000ms`
   - 目的：覆盖 TTS 播放尾巴、队列播放延迟、AEC 收敛残留。

4. 连续聊天预录缓存增加。
   - `CONT_PREROLL_CHUNKS`: `12 -> 16`
   - 目的：启动门槛变严后，仍尽量保留用户开头几个字。

5. 日志增强。
   - `continuous utterance start` 日志增加 `hits` 和 `sr`，方便判断到底是能量触发还是 ESP-SR VAD 参与。

## 验证结果

### Build / Flash

- build-only 通过。
- 烧录 COM3 成功。
- app 分区剩余：`0xd5770 bytes`，约 `28%`。
- 最新启动监控日志：
  - `D:\esp32\esp32idf-robot\debug_logs\monitor_20260506_214548.log`

### 启动链路

启动后确认：

- Wi-Fi 连接成功。
- WebSocket 连接 MCP `/esp32_ws` 成功。
- AFE/AEC 懒加载初始化成功：
  - `ready: input=RMNM rate=16000Hz aec=1 vad=software wake=0`
  - `continuous chat enabled path=afe_aec`

MCP 健康检查确认：

- `qq_language=mimo-v2.5-pro`
- `qq_tool=mimo-v2.5-pro`
- `esp32_voice=doubao-realtime-dialog`
- `doubao_dialog_enabled=true`
- `doubao_dialog_configured=true`

### 静置连续聊天测试

打开 `CHAT` 后静置约 50 秒：

- 多次出现 `sr=1`，但没有触发上传。
- 出现过单次强尖峰，例如：
  - `avg=1210 peak=4726`
- 由于新逻辑要求连续强证据，单次尖峰没有触发 `audio stream begin`。

结论：小噪声、偶发 ESP-SR VAD 命中、单次尖峰被压住了。

### 播放回声测试

连续聊天打开时下发 TTS 测试：

- 请求成功：
  - `/api/esp32/tts-test?text=连续聊天回声门控测试`
- ESP32 播放成功：
  - `tts stream end chunks=37`
  - `play tts wav=112684 rate=16000 ch=1`
  - `tts play finished ... ret=ESP_OK`
- 播放期间 busy gate 生效：
  - `cont vad gated busy local=1 mcp=1`
- 播放结束后 rearm 生效：
  - `rearm_left=2927ms`
- 播放后仍出现过强尖峰，例如：
  - `peak=6239 avg=1385`
  - `peak=5739 avg=2158`
- 这些尖峰没有连续满足启动条件，没有触发上传。

结论：本轮改动对 TTS 回声和播放残留的抑制明显改善。

## 当前缺点和风险

1. 还不是真正的全双工。
   - 当前体验仍是“VAD 判断一句话结束 -> 上传 -> 豆包返回 -> 播放”。
   - 不是边采边传、边听边说的低延迟全双工。

2. 近场/远场区分仍靠能量启发式。
   - 现在能压住多数静音、小噪声、单次尖峰。
   - 但如果房间里有人持续大声说话、电视/游戏语音足够响，仍可能被当成用户语音。
   - 这不是简单调阈值能彻底解决的问题，需要更完整的 AFE、声源方向、唤醒词、用户意图门控或更强的 AEC/NS。

3. 启动阈值变严后，轻声或远一点说话可能漏检。
   - 这是本轮为了减少误触发做出的权衡。
   - 预录缓存已加大，但如果用户说话太轻，可能需要靠近麦克风或切换到更灵敏档位。

4. AFE/AEC 还不是完整官方 ESP-SR AFE 方案。
   - 当前使用 ADF `aec_stream` 加软件 VAD/ESP-SR VAD 辅助。
   - 还没完整迁入 Korvo-2 官方 AFE/AEC/NS/WakeNet/MultiNet 例程形态。

5. TTS busy 生命周期还有边界风险。
   - 本轮 TTS 测试里，服务器发送完成和本地播放完成之间靠 `3s rearm` 覆盖。
   - 对很长的 legacy WAV TTS，最好让 busy 状态严格绑定 `audio_player` 实际播放完成，而不是只依赖服务端状态。

6. `sr_enabled:false` 仍容易误导。
   - 当前 telemetry 里该字段仍不能准确表达 AFE/AEC/ESP-SR VAD 的实际工作状态。
   - 后续应拆成 `afe_enabled`、`aec_enabled`、`wake_enabled`、`vad_enabled` 等字段。

7. 串口命令和音频回调存在小 race。
   - 在手动关闭 `CHAT` 的瞬间，如果刚好有强音频帧，可能出现一个极短的 `manual_stop` 会话。
   - 本轮观察到过 `duration=37ms` 的取消会话，未进入正常 ASR，但后续可以把“正在停止连续聊天”作为更早的门控状态。

8. 中文串口日志仍有乱码。
   - LVGL UI 本身不一定乱码，但串口 monitor 的中文输出仍会出现编码显示问题。
   - 这影响调试可读性，不影响语音主链路。

## 建议下一步

1. 增加连续聊天灵敏度档位。
   - 例如：安静房间 / 普通 / 近场强门槛。
   - UI 设置页可以暴露该选项，避免只有一组硬编码阈值。

2. 修正 busy 生命周期。
   - 让所有 TTS 下行路径都以 `audio_player` 实际播放开始/结束为准。
   - 避免长 TTS 播放超过 rearm 延迟后被误录。

3. 增加“停止中”门控。
   - 在 `stop_continuous_chat()` 开始时先置位 stopping flag。
   - `on_capture_audio()` 看到 stopping 就不允许新会话开始，消除 37ms 取消会话。

4. 深入迁入官方 ESP-SR AFE。
   - 对齐 Korvo-2 官方例程的麦克风输入格式、AEC reference、NS、VAD、WakeNet。
   - 当前 RMNM / ADF AEC 能跑，但还不是最终形态。

5. MCP 豆包 Dialog 改成更实时的流式上传。
   - 当前服务端仍依赖收尾静音和结束后处理。
   - 低延迟连续聊天应逐步改成边采边传、明确结束事件。

6. 建立固定回归脚本。
   - 静置 60 秒不能上传。
   - TTS 播放期间不能上传。
   - 近场说一句必须上传一次。
   - 播放后 3 秒内不能重启录音。

## 本轮状态

- 固件已烧录到 COM3。
- 连续聊天已手动关闭，避免后台继续听房间声。
- 发布仓库工作副本已同步源码改动。
- 未提交 Git。
- 未推 GitHub。
