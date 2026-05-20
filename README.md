# MCP-ESP32

这个仓库整理了当前 `D:\esp32` 工作目录中的核心项目代码，主要包含：

- `MCP-robot/`
  NapCat / ESP32 的异步中控服务，负责消息调度、模型调用、工具调用、视觉链路和 TTS。
- `esp32idf-robot/`
  ESP32-S3-Korvo-2 的 ESP-IDF/ADF 固件。当前语音链路为 SET 按键录音、WebSocket 连接 MCP、TTS 下行 WAV 播放。
- `GenericAgent/`
  作为 `root/` 项目级命令执行通道的通用 Agent。
- `bootmain/`
  当前本地环境里用于 NapCat 启动辅助的引导文件。

为了避免把本机环境和敏感信息一并上传，这个仓库刻意排除了以下内容：

- 虚拟环境、缓存目录、日志、运行时数据
- 本地密钥文件和临时输出
- Arduino IDE、ESP32 板卡包、QQ/NapCat 安装包和其他第三方二进制环境文件

## 复刻环境和依赖

如果你要在另一台机器复现，建议只拉这个仓库代码，然后在新机器上重新安装工具链和依赖。不要直接复制本机的 `build/`、`venv/`、`data/`、`debug_logs/`、`managed_components/` 或任何密钥文件。

### 必备硬件

- ESP32-S3-Korvo-2 V3
- USB 数据线和可用串口
- 一台和 ESP32 处在同一局域网/热点下的电脑

### 固件工具链

- ESP-IDF，建议使用 `v5.5.x`
- ESP-IDF Tools 中的 Python、CMake、Ninja、esptool 等工具
- 目标芯片设置为 `esp32s3`

`esp32idf-robot/main/idf_component.yml` 会通过 ESP Component Registry 拉取固件组件依赖：

- `espressif/esp-sr: 2.4.4`
- `lvgl/lvgl: ^8.4.0`
- `esp_lcd_touch_gt911`
- `esp_lcd_touch_tt21100`

### MCP 服务端依赖

进入 `MCP-robot` 后创建虚拟环境并安装依赖：

```powershell
python -m venv venv
.\venv\Scripts\activate
pip install -r requirements.txt
```

当前主要 Python 依赖包括：

- `fastapi`
- `uvicorn`
- `websockets`
- `openai`
- `httpx`
- `requests`
- `beautifulsoup4`

### 必须手动配置

1. 复制 `MCP-robot/local_keys.example.py` 为 `MCP-robot/local_keys.py`，填入豆包 Realtime Dialog、OpenAI 兼容接口和其他模型/工具 API Key。
2. 修改 `esp32idf-robot/main/app_config.h`：
   - Wi-Fi SSID
   - Wi-Fi 密码
   - MCP WebSocket 地址：`ws://电脑局域网IP:8080/esp32_ws`
3. 启动 MCP 服务端：

```powershell
cd MCP-robot
.\venv\Scripts\python.exe main.py
```

4. 编译烧录 ESP32：

```powershell
cd esp32idf-robot
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

5. 如果使用 NapCat/QQ，让 NapCat 连接 MCP 的 `/ws`；ESP32 固件连接 `/esp32_ws`。

### 不要提交的内容

- `local_keys.py`
- 真实 Wi-Fi、IP、API Key
- `build/`
- `venv/`
- `data/`
- `debug_logs/`
- `managed_components/`

当前 ESP32 播放端保持 `16kHz / 16bit / mono`，默认音量为 `85`。
