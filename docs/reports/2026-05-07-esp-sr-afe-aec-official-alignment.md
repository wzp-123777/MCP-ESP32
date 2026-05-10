# ESP-SR AFE/AEC 官方架构对齐报告

日期：2026-05-07

## 目标

把 ESP32-S3-Korvo-2 语音采集从旧的 AEC stream 思路正式收束到 ESP-SR 官方 AFE `feed/fetch` 架构：

- I2S 原始多通道输入进入 AFE `feed()`
- AFE `fetch()` 输出 16 kHz / 16-bit / mono 给 MCP
- 模型分区加载 WakeNet
- AFE VAD 负责连续聊天的说话开始/结束事件
- AEC reference 使用 Korvo-2 官方 `RMNM` 输入格式里的 `R` 通道

## 本轮完成

1. `afe_capture.c` 已使用官方 AFE 初始化链路：
   - `esp_srmodel_init("model")`
   - `afe_config_init(AFE_CAPTURE_INPUT_FMT, s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF)`
   - `esp_afe_handle_from_config()`
   - `create_from_config()`
   - 独立 `feed` / `fetch` 任务

2. Korvo-2 输入格式对齐官方：
   - `AFE_CAPTURE_INPUT_FMT = "RMNM"`
   - `R` 为播放参考信号
   - `M` 为麦克风信号
   - `N` 为未使用通道

3. AEC / VAD / WakeNet 配置已接入：
   - `aec_init = true`
   - `aec_mode = AEC_MODE_SR_HIGH_PERF`
   - `aec_filter_length = 4`
   - `vad_init = true`
   - WakeNet 从模型分区自动查找 `ESP_WN_PREFIX`
   - `CONFIG_SR_WN_WN9_HIESP=y`

4. `fetch` 侧进一步对齐官方 recorder 写法：
   - 从 `fetch_with_delay(..., 100ms)` 改为阻塞式 `fetch()`
   - 避免 fetch 任务主动轮询过快导致 AFE ringbuffer 空读噪声

5. 连续聊天的 AFE 路径已减少重复判定：
   - AFE/AEC 路径下，不再额外创建本地 `esp_vad` 去抢开始判定
   - 说话开始以 `AFE_CAPTURE_EVENT_VAD_START` 为准
   - 说话结束以 `AFE_CAPTURE_EVENT_VAD_END` 为主
   - 原能量判定只保留为说话中 watchdog / 兜底收尾

6. 发布副本已补齐 `afe_capture.c` / `afe_capture.h`：
   - `esp32idf-robot/main/CMakeLists.txt` 已引用这两个文件
   - 之前发布副本缺文件，单独编译会不完整

7. AFE VAD 误触发保护：
   - 旧逻辑：`AFE_CAPTURE_EVENT_VAD_START` 立即 `audio_stream_begin`
   - 新逻辑：AFE VAD start 只进入 pending，后续 PCM 在 1200ms 窗口内通过能量确认才开始上传
   - 确认门限：`avg_abs >= max(240, noise_floor + 160)` 且 `peak >= 900`，连续 2 次命中
   - 目的：挡住低能量 AFE 假开始，避免 MCP 收到低 RMS 空录音后返回 `assistant_error`

8. `sr_enabled` telemetry 修正：
   - `mcp_client_send_telemetry()` 不再硬编码 `sr_enabled:false`
   - 新增 `mcp_client_set_sr_enabled(bool enabled)`
   - AFE 初始化成功后设置为 true

## 验证状态

### Build-only

命令：

```powershell
powershell -ExecutionPolicy Bypass -File C:\Users\wuzuping\.codex\skills\esp32-idf-debug\scripts\esp32_debug_cycle.ps1 -NoFlash -MonitorSeconds 0
```

结果：

- 编译通过
- 生成 `esp32idf_robot_voice.bin`
- app 大小：`0x277700`
- factory app 分区：`0x300000`
- 剩余：`0x88900`，约 18%
- `srmodels.bin` 大小：291,142 bytes

唯一编译警告仍是 IDF 旧 I2S driver deprecation warning，当前 ADF `i2s_stream` 仍依赖这套接口，暂不作为功能阻断。

最新 build log：

- `D:\esp32\esp32idf-robot\debug_logs\build_20260507_183051.log`

### Flash / monitor

用户重新连接硬件后，`COM3` 已恢复为 present 的 CP210x 串口，后续烧录成功：

- chip：ESP32-S3 revision v0.2
- PSRAM：8MB
- app：`0x277910`
- factory app 分区剩余：`0x886f0`，约 18%
- `srmodels.bin` 已写入 `0x310000`
- MCP WebSocket 已连通：`websocket connected` / `config ack`

本地固件 `app_config.h` 临时指向当前电脑 WLAN IP，发布仓库 `app_config.h` 仍保持占位符。

最新烧录/监控日志：

- `D:\esp32\esp32idf-robot\debug_logs\flash_20260507_193902.log`
- `D:\esp32\esp32idf-robot\debug_logs\monitor_20260507_193902.log`

### AFE 实机结果

串口命令进入 `CHAT` 后，AFE/AEC 官方链路已经真实启动：

- `loaded 1 ESP-SR model(s)`
- WakeNet：`wn9_hiesp`
- `feed task ready bytes=8192 samples=1024 channels=4`
- `fetch task ready samples=512 channels=1`
- `ready: input=RMNM rate=16000Hz feed=4ch/1024 samples fetch=1ch/512 samples aec=1 vad=esp-sr wake=1`
- `continuous chat enabled path=afe_aec`
- 160 秒静音观察内未见 `Ringbuffer of AFE is empty`
- 未见 panic / Guru Meditation

关键日志：

- `D:\esp32\esp32idf-robot\debug_logs\serial_chat_energy_gate_live_20260507_193327.log`

### 误触发修复验证

修复前，AFE 低能量假触发会让 MCP 收到短音频：

- MCP 记录 `rms=150.49 peak=812 min_rms=220.00`
- MCP 记录 `rms=115.09 peak=464 min_rms=220.00`
- 这类音频会在豆包前被 MCP 丢弃，ESP32 收到 `assistant_error`

修复后，静音环境下进入连续聊天 160 秒：

- AFE fetch 稳定输出
- `vad=0`
- 未触发 `audio_stream_begin`
- 未见新的低 RMS `assistant_error`

真人说话通过能量确认的验证仍需下一轮让用户在采集窗口内实际说话。

## 当前缺点 / 风险

1. internal heap 非常紧
   - AFE 初始化前：internal free 约 17,775 bytes，largest 约 8,704 bytes
   - AFE 初始化后：internal free 约 1,087 bytes，largest 约 704 bytes
   - 这说明功能能跑，但运行余量危险；后续任何新增内部 RAM 分配都可能失败。

2. 真人语音通过率还没闭环
   - 静音误触发已明显改善。
   - 但本轮没有稳定抓到用户真人说话后的 `afe utterance start -> Doubao ASR/CHAT/TTS` 全链路。
   - 下一轮需要对板子说 2-3 句短话，确认门限不会过紧。

3. 还不是真正自然的全双工
   - 当前策略仍是“播放中抑制新录音 / 播放后延迟重启监听”。
   - AEC 已有 reference 通道，但 barge-in 还没有产品级状态机。
   - 用户打断机器人说话时，是否应该立刻停 TTS、切录音、上传新话，需要再设计。

4. Wi-Fi / 热点稳定性影响调试
   - 实机日志中出现过 `wifi disconnected reason=200/201`
   - RSSI 曾降到约 -75 后 WebSocket 断开
   - 之后可自动恢复，但调试连续聊天时会打断链路

5. `sr_enabled` telemetry 修正已实现，但还需最终确认
   - 固件已新增动态上报。
   - 因串口打开会导致板子重连，最后一轮没有稳定在新固件上完成 `CHAT -> telemetry sr_enabled:true` 的日志确认。

6. MCP 上传仍偏“句子级”
   - ESP32 已经可以边采边发 chunk。
   - 但整体交互仍以 VAD 开始/结束后的 utterance 为单位。
   - 要继续降低延迟，需要 MCP 到豆包 Dialog 也进一步改成更彻底的实时流式上传。

7. 发布仓库还有未完成的 MCP 文本链路改动
   - 当前发布副本里已有 NapCat/QQ 走 MiMo、ESP32 语音走豆包的路由改动。
   - 本轮没有改这条链路，只保持这个原则。

## 下一步建议

1. 下一轮实机语音验证：
   - AFE 初始化 heap
   - `feed task ready`
   - `fetch task ready`
   - `ready: input=RMNM`
   - WakeNet 模型加载
   - `afe vad start/end`
   - 是否还有 ringbuffer empty warning
   - 真人短句是否能通过能量确认并完整跑到 Doubao ASR/CHAT/TTS

2. 做 barge-in 状态机：
   - 播放中检测到稳定近端人声
   - 停止当前 TTS 播放
   - 切换到录音上传
   - MCP 侧取消或忽略上一轮残余 TTS

3. 继续压内存：
   - 统计 AFE 创建前后 internal heap / largest block
   - 评估 LVGL buffer、音频 ringbuffer、任务栈是否还能降一点

4. 豆包 Dialog 链路继续流式化：
   - 现在短句尾静音已经解决
   - 但真正低延迟仍应靠实时上传，不靠更长 tail silence
