from __future__ import annotations

import asyncio
import base64
from pathlib import Path

from app_config import load_config
from models import TTSModelService
from runtime import _select_single_wav_from_base64


async def main() -> None:
    config = load_config()
    service = TTSModelService(config.tts_model, config.tts_voice, config.tts_style_prompt)
    out_dir = config.data_dir / "esp32_tts"
    out_dir.mkdir(parents=True, exist_ok=True)
    text = "小乐测试语音，当前使用二点五版本模型。"
    print(f"model={config.tts_model.model} voice={config.tts_voice} style={config.tts_style_prompt}")
    async for audio in service.stream_audio(text):
        audio_b64 = str(audio.get("audio_b64") or "")
        if not audio_b64:
            continue
        esp32_b64 = _select_single_wav_from_base64(audio_b64)
        wav = base64.b64decode(esp32_b64 or audio_b64)
        out = out_dir / "manual_tts_test.wav"
        out.write_bytes(wav)
        print(f"saved={out} bytes={len(wav)} head={wav[:12]!r}")
        return
    raise SystemExit("no audio returned")


if __name__ == "__main__":
    asyncio.run(main())
