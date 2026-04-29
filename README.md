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

如果你要在另一台机器复现：

1. 分别为 `MCP-robot` 和 `GenericAgent` 安装依赖
2. 复制 `MCP-robot/local_keys.example.py` 为 `MCP-robot/local_keys.py`，按需补充模型和工具 API Key
3. 启动 `MCP-robot/main.py`
4. 修改 `esp32idf-robot/main/app_config.h` 里的 Wi-Fi 和 MCP WebSocket 地址
5. 用 ESP-IDF/ADF 编译烧录 `esp32idf-robot`
6. 让 NapCat 连接 `/ws`，ESP32 连接 `/esp32_ws`

当前 ESP32 播放端采用 `16kHz / 16bit / mono`，默认音量较保守，避免 ES8311/功放/小扬声器过载失真。
