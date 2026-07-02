# esp32idf-robot

ESP-IDF + ESP-ADF 语音机器人工程，目标硬件为 ESP32-S3-Korvo-2 V3。启动后会连接 Wi-Fi 和 MCP WebSocket，按住 `SET` 录音，松开发送到 MCP；服务端 TTS WAV 下行后通过 ES8311 播放。

## 可复现环境

| 项目 | 版本/型号 |
| --- | --- |
| 硬件 | ESP32-S3-Korvo-2 V3 |
| 芯片目标 | `esp32s3` |
| Flash | 16 MB |
| ESP-IDF | v5.5.3 |
| ESP-ADF | v2.8，本地验证 checkout `d049321` |
| ESP-SR | 2.4.6 |

`main/idf_component.yml` 固定了关键托管组件：

```yaml
dependencies:
  espressif/esp-sr: "2.4.6"
  lvgl/lvgl: "^8.4.0"
  esp_lcd_touch_gt911: "^1"
  esp_lcd_touch_tt21100: ">=1.0.0"
```

## 公开配置

公开版本的 `main/app_config.h` 只保留占位符：

```c
#define ROBOT_WIFI_SSID "YOUR_WIFI_SSID"
#define ROBOT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define ROBOT_MCP_URI "ws://YOUR_MCP_SERVER_IP:8080/esp32_ws"
#define ROBOT_DEVICE_ID "ESP32_KORVO_2"
```

本地运行时把 `YOUR_WIFI_SSID`、`YOUR_WIFI_PASSWORD` 和 `YOUR_MCP_SERVER_IP` 改成自己的环境。不要把真实配置提交到公开仓库。

## 安装 ESP-IDF / ESP-ADF

Windows PowerShell 示例：

```powershell
git clone --recursive -b v2.8 https://github.com/espressif/esp-adf.git D:\Espressif\esp-adf-v2.8
cd D:\Espressif\esp-adf-v2.8\esp-idf
git fetch --tags
git checkout v5.5.3
.\install.ps1 esp32s3
.\export.ps1
$env:ADF_PATH='D:\Espressif\esp-adf-v2.8'
idf.py --version
```

构建前必须设置 `ADF_PATH`，因为根 `CMakeLists.txt` 通过 `$ENV{ADF_PATH}/CMakeLists.txt` 引入 ESP-ADF 组件。

## 构建和烧录

```powershell
cd <本仓库>\esp32idf-robot
$env:IDF_CCACHE_ENABLE='0'
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

如果只想编译不烧录：

```powershell
idf.py build
```

也可以使用脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash_monitor.ps1 -Port COM3
powershell -ExecutionPolicy Bypass -File .\build_flash_monitor.ps1 -NoFlash -NoMonitor
```

## 按键

- `SET` 按住：开始录音
- `SET` 松开：结束录音并发送 MCP
- `VOL+` / `VOL-`：调节音量
- `PLAY`：本地播放一次“小乐”测试音
- `MODE`：重新触发 MCP 连接

## 串口命令

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

## 连续聊天和全双工

当前连续聊天仍走 `mic_diag` 的 16 kHz / 16-bit / mono PCM 采集，并使用轻量 VAD 断句。工程中保留 ESP-SR AFE/AEC 迁移记录，后续可继续对齐官方 AFE feed/fetch、WakeNet/VAD 和 ES7210 reference channel。

## 提交注意

公开 GitHub 保留占位符配置即可；比赛现场可运行包可以保留真实 Wi-Fi 和 `local_keys.py`，但不要上传公开仓库。
