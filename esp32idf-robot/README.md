# esp32idf-robot

ESP-IDF + ESP-ADF 语音机器人工程，目标硬件为 ESP32-S3-Korvo-2 V3。

当前默认不再循环播放“小乐”。启动后会自动连接 Wi-Fi 和 MCP WebSocket，按住 `SET` 录音，松开发送到 MCP，服务端 TTS WAV 下行后通过 ES8311 播放。

默认配置在 `main/app_config.h` 中设置。发布仓库使用占位符，本地工作副本可以填自己的 Wi-Fi 和 MCP 地址：

- Wi-Fi：`YOUR_WIFI_SSID`
- MCP：`ws://YOUR_MCP_SERVER_IP:8080/esp32_ws`
- 设备：`ESP32_KORVO_2`

## 连续聊天和全双工

当前连续聊天仍走 `mic_diag` 的 16 kHz / 16-bit / mono PCM 采集，并使用轻量 VAD 断句。已经加入动态噪声底和连续静音判断，避免单个峰值或播放回声让“说完了”一直不结束。

ESP32-S3-Korvo-2 硬件支持全双工 AEC，但不能只改阈值实现。官方路线是切到 ESP-SR AFE：

- AEC 示例：`$ADF_PATH/examples/advanced_examples/aec/main/aec_examples.c`
- WakeNet/VAD 示例：`$ADF_PATH/examples/speech_recognition/wwe/main/main.c`
- 实时通信示例：`$ADF_PATH/examples/ai_agent/volc_rtc/components/audio_processor/`

主工程里 `main/afe_full_duplex_plan.h` 记录了迁移入口，默认 `ROBOT_AFE_FULL_DUPLEX_EXPERIMENTAL=0`。真正启用前需要先增加 `model` 分区、选择 ESP-SR/WakeNet 模型，并把采集入口从 mono PCM 替换成带 ES7210 reference channel 的 AFE feed/fetch。

## 构建和烧录

首次构建前请先安装 ESP-IDF `v5.5.x`，并设置目标芯片：

```powershell
idf.py set-target esp32s3
```

固件组件依赖由 `main/idf_component.yml` 管理，构建时会自动下载：

- `espressif/esp-sr: 2.4.4`
- `lvgl/lvgl: ^8.4.0`
- `esp_lcd_touch_gt911`
- `esp_lcd_touch_tt21100`

```powershell
powershell -ExecutionPolicy Bypass -File D:\esp32\esp32idf-robot\build_flash_monitor.ps1 -Port COM3
```

如果只想编译不烧录：

```powershell
powershell -ExecutionPolicy Bypass -File D:\esp32\esp32idf-robot\build_flash_monitor.ps1 -NoFlash -NoMonitor
```

## 按键

- `SET` 按住：开始录音
- `SET` 松开：结束录音并发送 MCP
- `VOL+` / `VOL-`：调节音量
- `PLAY`：本地播放一次“小乐”测试音
- `MODE`：重新触发 MCP 连接

## 串口命令

串口仍保留为备用调试入口，但主流程不依赖串口输入。

- `PLAY` / `XIAOLE`：播放一次
- `LOOP`：循环播放
- `STOP`：停止循环，当前一句播完后停止
- `MIC ON`：开启麦克风电平诊断，串口每秒输出 `peak/avg`
- `MIC OFF`：关闭麦克风电平诊断
- `VOL 80`：设置音量 0-100
- `VOL+` / `VOL-`：音量加减
- `MCP URL <url>`：设置 MCP 服务地址
- `MCP CONNECT`：启动 Wi-Fi/MCP 连接
- `HELP`：打印命令
