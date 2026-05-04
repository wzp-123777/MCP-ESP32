# esp32idf-robot

ESP-IDF + ESP-ADF 语音机器人工程，目标硬件为 ESP32-S3-Korvo-2 V3。

当前默认不再循环播放“小乐”。启动后会自动连接 Wi-Fi 和 MCP WebSocket，按住 `SET` 录音，松开发送到 MCP，服务端 TTS WAV 下行后通过 ES8311 播放。

默认配置在 `main/app_config.h` 中使用占位符。请在本地工作副本里改成自己的 Wi-Fi 和 MCP 地址，不要提交真实配置：

- Wi-Fi：`YOUR_WIFI_SSID`
- MCP：`ws://YOUR_MCP_SERVER_IP:8080/esp32_ws`
- 设备：`ESP32_KORVO_2`

## 构建和烧录

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
