# ESP32 人设、记忆和音色配置位置

## 总体链路

ESP32 固件只负责选择 `persona_id` 和 `voice_id`，并把它们通过 `client_config` 发给 MCP。

MCP 服务端负责：

- 根据 `persona_id` 读取人设文件。
- 把结构化记忆和近期摘要拼进豆包 Realtime Dialog 的 `system_role`。
- 根据 `voice_id` 查音色预设，并把 speaker 写入豆包 StartSession 的 `tts.speaker`。

## 固件端入口

文件：

- `esp32idf-robot/main/robot_voice_main.c`

当前固件内置的人设选项：

```c
static const config_option_t s_personas[] = {
    {"default", "默认人设"},
    {"sweet", "甜妹人设"},
    {"serious", "认真助手"},
};
```

当前固件内置的音色选项：

```c
static const config_option_t s_voice_profiles[] = {
    {"default", "默认音色"},
    {"female_soft", "柔和女声"},
    {"female_bright", "明亮女声"},
};
```

切换方式：

- 屏幕设置页点击人设/音色按钮。
- 串口命令：`PERSONA`
- 串口命令：`VOICE`

固件发送配置的位置：

```c
mcp_client_send_config(persona->id, persona->label, voice->id, voice->label, ...);
```

## MCP 人设位置

默认目录：

- `MCP-robot/data/personas`

可用环境变量覆盖：

- `MCP_PERSONA_DIR`

服务端读取逻辑：

- `MCP-robot/runtime.py`
- `_load_persona_prompt(persona_id)`
- 先找 `<persona_id>.md`
- 再找 `<persona_id>.txt`
- `default` 不读取额外文件

当前本机已有：

- `MCP-robot/data/personas/sweet.md`
- `MCP-robot/data/personas/serious.md`

新增人设步骤：

1. 在固件 `s_personas` 里加一项，例如 `{"teacher", "老师人设"}`。
2. 在 MCP 侧创建 `MCP-robot/data/personas/teacher.md`。
3. 重启或重新连接 ESP32，让 `client_config` 更新。

## MCP 记忆位置

默认数据目录：

- `MCP-robot/data`

可用环境变量覆盖：

- `MCP_ROBOT_DATA_DIR`

主要记忆文件：

- `MCP-robot/data/conversation_history.jsonl`
- `MCP-robot/data/conversation_history_summaries.jsonl`
- `MCP-robot/data/conversation_history_embeddings.jsonl`
- `MCP-robot/data/structured_memory.jsonl`
- `MCP-robot/data/user_profiles.jsonl`
- `MCP-robot/data/subconscious_memory.jsonl`

初始化位置：

- `MCP-robot/runtime.py`
- `RobotRuntime.__init__`

ESP32 豆包语音链路拼 prompt 的位置：

- `MCP-robot/runtime.py`
- `_build_esp32_dialog_system_role(...)`

这里会组合：

- `DOUBAO_DIALOG_SYSTEM_ROLE`
- 当前人设文件内容
- `StructuredMemoryStore.build_context(...)`
- 最近对话摘要
- 实时语音短句要求

## 豆包 Realtime Dialog 音色

默认配置位置：

- `MCP-robot/app_config.py`
- `DoubaoDialogConfig.tts_speaker`

默认环境变量：

- `DOUBAO_DIALOG_TTS_SPEAKER`

当前默认值：

```python
DOUBAO_DIALOG_TTS_SPEAKER = "zh_female_vv_jupiter_bigtts"
```

按 `voice_id` 覆盖音色的位置：

- `MCP-robot/data/voice_presets.json`

可用环境变量覆盖：

- `MCP_VOICE_PRESET_FILE`

服务端读取逻辑：

- `MCP-robot/runtime.py`
- `_load_voice_presets()`
- `_voice_speaker_for_config(...)`

当前本机 `voice_presets.json` 内容是：

```json
{
  "female_soft": "",
  "female_bright": ""
}
```

这表示固件能切换 `female_soft` / `female_bright`，但 MCP 侧还没有给它们绑定真实豆包 speaker；因此会回退到默认 `DOUBAO_DIALOG_TTS_SPEAKER`。

正确写法示例：

```json
{
  "female_soft": {
    "speaker": "zh_female_vv_jupiter_bigtts"
  },
  "female_bright": {
    "speaker": "zh_female_qingxin"
  }
}
```

也支持简写：

```json
{
  "female_soft": "zh_female_vv_jupiter_bigtts"
}
```

豆包 Realtime Dialog 发送位置：

- `MCP-robot/doubao_dialog.py`
- `run_pcm_dialog(...)`

StartSession payload 里设置：

```python
"tts": {
    "speaker": tts_speaker_override.strip() or self.config.tts_speaker,
    "audio_config": {
        "channel": self.config.tts_channel,
        "format": self.config.tts_format,
        "sample_rate": self.config.tts_sample_rate,
    }
}
```

当前服务端会向豆包请求 `24000Hz / pcm_s16le / mono`，再重采样为 ESP32 播放需要的 `16000Hz / 16bit / mono`。

## MiMo TTS 音色

QQ/NapCat 文本链路保留 MiMo。MiMo TTS 与 ESP32 豆包语音链路是两套配置，不要混用。

MiMo TTS 配置位置：

- `MCP-robot/app_config.py`
- `TTS_PRESETS`
- `MIMO_TTS_PRESET`
- `MIMO_TTS_VOICE`
- `MIMO_TTS_STYLE_PROMPT`

MiMo TTS 实现位置：

- `MCP-robot/models.py`
- `TTSModelService`
- `_mimo_audio_options(...)`
- `_mimo_tts_messages(...)`

内置 MiMo 预设包括：

- `clear_female`
- `sweet_female`
- `soft_female`
- `bright_female`
- `calm_male`

注意：ESP32 语音链路必须继续使用豆包 Realtime Dialog；MiMo 音色只用于 QQ/NapCat 文本链路或旧的非 Dialog TTS 兜底。
