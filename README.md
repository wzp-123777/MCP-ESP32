# MCP-ESP32

这个仓库整理了当前 `D:\esp32` 工作目录中的核心项目代码，主要包含：

- `MCP-robot/`
  NapCat / ESP32 的异步中控服务，负责消息调度、模型调用、工具调用、视觉链路和 TTS。
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
2. 按需补充模型和工具 API Key
3. 启动 `MCP-robot/main.py`
4. 让 NapCat 连接 `/ws`，ESP32 连接 `/esp32_ws`
