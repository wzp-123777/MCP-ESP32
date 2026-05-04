from __future__ import annotations

import asyncio
import base64
import html
import io
import json
import logging
import math
import os
import re
import struct
import time
import unicodedata
import wave
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Protocol

import httpx
from approval import ApprovalManager
from app_config import AppConfig
from conversation_store import ConversationStore
from doubao_dialog import (
    EVENT_ASR_ENDED,
    EVENT_ASR_INFO,
    EVENT_ASR_RESPONSE,
    EVENT_CHAT_ENDED,
    EVENT_CHAT_RESPONSE,
    EVENT_SESSION_FAILED,
    EVENT_TTS_ENDED,
    EVENT_TTS_RESPONSE,
    EVENT_TTS_SENTENCE_END,
    EVENT_TTS_SENTENCE_START,
    DoubaoRealtimeDialogClient,
)
from frame_store import FrameStore
from generic_agent_bridge import GenericAgentBridge
from memory_store import SubconsciousMemoryStore
from structured_memory_store import StructuredMemoryStore
from models import (
    ASRModelService,
    ContextEmbeddingService,
    LanguageModelService,
    SemanticPlan,
    ToolPlan,
    ToolModelService,
    TTSModelService,
    VisionAnalysis,
    VisionModelService,
)
from task_manager import BackgroundTaskManager
from tools import AmapTool, DashScopeQuarkSearchTool, HighResVisionTool, SeniverseWeatherTool, ToolExecutionBundle, ToolRegistry


logger = logging.getLogger("MCP_Robot.Runtime")
trace_logger = logging.getLogger("MCP_Robot.Trace")


class ConnectionManagerProtocol(Protocol):
    async def send_to_qq(self, message: dict[str, Any]) -> None: ...

    async def send_to_esp32(self, message: dict[str, Any]) -> None: ...


async def _send_esp32_status(
    connection_manager: ConnectionManagerProtocol,
    *,
    status: str,
    device_id: str = "",
    session_id: str = "",
    text: str = "",
) -> None:
    payload: dict[str, Any] = {"type": "assistant_status", "status": status}
    if device_id:
        payload["device_id"] = device_id
    if session_id:
        payload["session_id"] = session_id
    if text:
        payload["text"] = text[:120]
    await connection_manager.send_to_esp32(payload)


@dataclass(slots=True)
class TextRequest:
    source: str
    text: str
    user_id: str = ""
    device_id: str = ""
    message_type: str = "private"
    group_id: str | None = None
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class PendingImageJob:
    chat_key: str
    session_id: str
    source: str
    message_type: str
    frame_id: str
    user_id: str = ""
    group_id: str | None = None
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    status: str = "pending"
    latest_query: str = ""
    waiting_for_followup: bool = False
    hold_message_sent: bool = False
    completion_sent: bool = False


@dataclass(slots=True)
class RequestContextBundle:
    session_id: str
    conversation_context: str
    latest_frame_context: str
    subconscious_context: str
    temporal_context: str = ""
    temporal_gap_minutes: float = 0.0
    recent_frame: Any | None = None
    referential_followup: bool = False


@dataclass(slots=True)
class OfflineGapAnalysisCandidate:
    session_id: str
    source: str
    gap_minutes: float
    last_turn_timestamp: str
    now_timestamp: str
    pre_gap_entries: list[dict[str, Any]]
    pre_gap_summaries: list[dict[str, Any]]
    return_user_text: str


@dataclass(slots=True)
class PendingAudioStream:
    session_id: str
    device_id: str
    pcm_path: Path
    sample_rate: int
    sample_bits: int
    channels: int
    encoding: str
    started_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    bytes_received: int = 0


@dataclass(slots=True)
class ESP32RuntimeConfig:
    persona_id: str = "default"
    persona_label: str = "默认人设"
    voice_id: str = "default"
    voice_label: str = "默认音色"
    continuous_chat: bool = False
    wake_enabled: bool = False
    wake_word: str = "doubao"
    updated_at: float = field(default_factory=time.time)


def _analyze_pcm_s16le(pcm_bytes: bytes) -> dict[str, float | int]:
    frame_bytes = len(pcm_bytes) - (len(pcm_bytes) % 2)
    if frame_bytes <= 0:
        return {"sample_count": 0, "nonzero_samples": 0, "nonzero_ratio": 0.0, "peak": 0, "rms": 0.0}
    sample_count = frame_bytes // 2
    nonzero_samples = 0
    peak = 0
    sum_squares = 0.0
    for (sample,) in struct.iter_unpack("<h", pcm_bytes[:frame_bytes]):
        if sample:
            nonzero_samples += 1
        amplitude = abs(sample)
        if amplitude > peak:
            peak = amplitude
        sum_squares += float(sample) * float(sample)
    return {
        "sample_count": sample_count,
        "nonzero_samples": nonzero_samples,
        "nonzero_ratio": nonzero_samples / sample_count if sample_count else 0.0,
        "peak": peak,
        "rms": math.sqrt(sum_squares / sample_count) if sample_count else 0.0,
    }


def _analyze_wav_audio(wav_bytes: bytes) -> dict[str, float | int]:
    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wav_file:
            frames = wav_file.readframes(wav_file.getnframes())
        return _analyze_pcm_s16le(frames)
    except Exception:
        return {"sample_count": 0, "nonzero_samples": 0, "nonzero_ratio": 0.0, "peak": 0, "rms": 0.0}


_TTS_AUDIO_B64_CHUNK_CHARS = 4096
_TTS_AUDIO_CHUNK_DELAY_SECONDS = 0.005
_TTS_STRONG_PUNCT_MIN_CHARS = 2
_TTS_SOFT_PUNCT_MIN_CHARS = 8
_TTS_MAX_SEGMENT_CHARS = 18
_ESP32_TTS_SAMPLE_RATE = 16000
_ESP32_TTS_MAX_SECONDS = 12.0
_ESP32_TTS_TARGET_PEAK = 12000
_ESP32_TTS_SOFT_LIMIT = 26000
_DIALOG_TTS_TARGET_PEAK = 14000
_DIALOG_TTS_MAX_GAIN = 8.0
_TTS_DEBUG_DIR = Path(__file__).resolve().parent / "data" / "esp32_tts"
_EMOJI_RE = re.compile(
    "["
    "\U0001F1E6-\U0001F1FF"
    "\U0001F300-\U0001F5FF"
    "\U0001F600-\U0001F64F"
    "\U0001F680-\U0001F6FF"
    "\U0001F700-\U0001F77F"
    "\U0001F780-\U0001F7FF"
    "\U0001F800-\U0001F8FF"
    "\U0001F900-\U0001F9FF"
    "\U0001FA70-\U0001FAFF"
    "\u2600-\u26FF"
    "\u2700-\u27BF"
    "\uFE0E-\uFE0F"
    "\u200D"
    "]+",
    re.UNICODE,
)
_ASCII_EMOTICON_RE = re.compile(
    r"(?<!\w)(?:[:;=8xX][\-o*']?[\)\]\(\[dDpP/:\}\{@\|\\]|[\(\[][\-o*']?[:;=8xX]|Q[AQ]|QAQ|TAT|T_T|orz|ORZ)(?!\w)"
)
_KAOMOJI_RE = re.compile(
    r"[\(（][^()\n（）]{0,30}(?:[\^_><;；=]|[｡。·•・ωдД▽△∀ヮ益╯╰ノﾉヽづつっ])[^\n()（）]{0,30}[\)）]"
)
_SPOKEN_PUNCTUATION = set("，。！？；：、,.!?;:（）()《》“”\"' -")


def _sanitize_spoken_text(text: str) -> str:
    cleaned = unicodedata.normalize("NFKC", str(text or ""))
    cleaned = re.sub(r"```[\s\S]*?```", " ", cleaned)
    cleaned = re.sub(r"`([^`]*)`", r"\1", cleaned)
    cleaned = cleaned.replace("～", "，").replace("~", "，")
    cleaned = cleaned.replace("…", "，").replace("⋯", "，")
    cleaned = _EMOJI_RE.sub("", cleaned)
    cleaned = _KAOMOJI_RE.sub("", cleaned)
    cleaned = _ASCII_EMOTICON_RE.sub("", cleaned)
    cleaned = re.sub(r"[*_#>\[\]{}|]+", " ", cleaned)

    spoken_chars: list[str] = []
    for char in cleaned:
        if char in _SPOKEN_PUNCTUATION:
            spoken_chars.append(char)
            continue
        category = unicodedata.category(char)
        if category[0] in {"L", "N", "Z"}:
            spoken_chars.append(char)
        else:
            spoken_chars.append(" ")

    cleaned = "".join(spoken_chars)
    cleaned = re.sub(r"[\r\n\t]+", " ", cleaned)
    cleaned = re.sub(r"\s*([，。！？；：、,.!?;:])\s*", r"\1", cleaned)
    cleaned = re.sub(r"[，,]{2,}", "，", cleaned)
    cleaned = re.sub(r"[。]{2,}", "。", cleaned)
    cleaned = re.sub(r"[!！]{2,}", "！", cleaned)
    cleaned = re.sub(r"[?？]{2,}", "？", cleaned)
    cleaned = cleaned.replace(",", "，")
    cleaned = cleaned.replace("!", "！")
    cleaned = cleaned.replace("?", "？")
    cleaned = cleaned.replace(";", "；")
    cleaned = cleaned.replace(":", "：")
    cleaned = re.sub(r"(?<!\d)\.(?!\d)", "。", cleaned)
    cleaned = re.sub(r"\s{2,}", " ", cleaned)
    return cleaned.strip(" ，,;；:：")


def _pcm_to_wav(pcm: bytes, *, sample_rate: int, channels: int = 1, bits_per_sample: int = 16) -> bytes:
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    header = (
        b"RIFF"
        + (36 + len(pcm)).to_bytes(4, "little")
        + b"WAVEfmt "
        + (16).to_bytes(4, "little")
        + (1).to_bytes(2, "little")
        + channels.to_bytes(2, "little")
        + sample_rate.to_bytes(4, "little")
        + byte_rate.to_bytes(4, "little")
        + block_align.to_bytes(2, "little")
        + bits_per_sample.to_bytes(2, "little")
        + b"data"
        + len(pcm).to_bytes(4, "little")
    )
    return header + pcm


def _clip16(value: float | int) -> int:
    return max(-32768, min(32767, int(round(value))))


def _soft_limit_sample(value: float, limit: int = _ESP32_TTS_SOFT_LIMIT) -> int:
    if limit <= 0:
        return _clip16(value)
    if value > limit:
        value = limit + (value - limit) * 0.25
    elif value < -limit:
        value = -limit + (value + limit) * 0.25
    return _clip16(value)


def _pcm_s16le_to_mono_samples(pcm: bytes, source_channels: int) -> list[float]:
    sample_count = len(pcm) // 2
    if sample_count <= 0 or source_channels not in (1, 2):
        return []
    samples = struct.unpack("<" + "h" * sample_count, pcm[: sample_count * 2])
    source_frames = sample_count // source_channels
    if source_frames <= 0:
        return []
    if source_channels == 1:
        return [float(samples[index]) for index in range(source_frames)]
    return [
        (float(samples[frame * 2]) + float(samples[frame * 2 + 1])) * 0.5
        for frame in range(source_frames)
    ]


def _remove_dc(samples: list[float]) -> list[float]:
    if not samples:
        return []
    mean = sum(samples) / len(samples)
    if abs(mean) < 1.0:
        return list(samples)
    return [sample - mean for sample in samples]


def _remove_dc_and_filter(samples: list[float], sample_rate: int) -> list[float]:
    if not samples or sample_rate <= 0:
        return []
    return _remove_dc(samples)


def _resample_float_mono(samples: list[float], *, source_rate: int, target_rate: int) -> list[float]:
    if not samples or source_rate <= 0 or target_rate <= 0:
        return []
    if source_rate == target_rate:
        return list(samples)
    target_frames = max(1, int(len(samples) * target_rate / source_rate))
    output: list[float] = []
    for frame in range(target_frames):
        pos = frame * source_rate / target_rate
        index = int(pos)
        frac = pos - index
        a = samples[min(index, len(samples) - 1)]
        b = samples[min(index + 1, len(samples) - 1)]
        output.append(a + (b - a) * frac)
    return output


def _finish_tts_pcm(samples: list[float], *, target_peak: int = _ESP32_TTS_TARGET_PEAK, max_gain: float = 1.0) -> bytes:
    if not samples:
        return b""
    peak = max(abs(sample) for sample in samples)
    if peak <= 0:
        return b""
    gain = min(max(0.01, max_gain), max(0.01, target_peak) / peak)
    output = bytearray(len(samples) * 2)
    for index, sample in enumerate(samples):
        struct.pack_into("<h", output, index * 2, _soft_limit_sample(sample * gain))
    return bytes(output)


def _resample_pcm_s16le_mono(
    pcm: bytes,
    *,
    source_rate: int,
    source_channels: int,
    target_rate: int,
    target_peak: int = _ESP32_TTS_TARGET_PEAK,
    max_gain: float = 1.0,
) -> bytes:
    if source_rate <= 0 or target_rate <= 0 or source_channels not in (1, 2):
        return b""
    mono = _pcm_s16le_to_mono_samples(pcm, source_channels)
    filtered = _remove_dc_and_filter(mono, source_rate)
    resampled = _resample_float_mono(filtered, source_rate=source_rate, target_rate=target_rate)
    return _finish_tts_pcm(resampled, target_peak=target_peak, max_gain=max_gain)


def _normalize_wav_for_esp32(audio_bytes: bytes) -> bytes:
    try:
        with wave.open(io.BytesIO(audio_bytes), "rb") as wav_file:
            source_channels = wav_file.getnchannels()
            source_rate = wav_file.getframerate()
            bits_per_sample = wav_file.getsampwidth() * 8
            source_frames = wav_file.getnframes()
            max_source_frames = int(source_rate * _ESP32_TTS_MAX_SECONDS) if source_rate > 0 else source_frames
            frames_to_read = min(source_frames, max_source_frames)
            pcm = wav_file.readframes(frames_to_read)
    except Exception as exc:
        logger.warning("TTS WAV normalize failed: %s", exc)
        return b""
    if bits_per_sample != 16 or source_channels not in (1, 2) or source_rate <= 0:
        logger.warning(
            "TTS WAV cannot normalize: rate=%d channels=%d bits=%d",
            source_rate,
            source_channels,
            bits_per_sample,
        )
        return b""
    if source_frames > frames_to_read:
        logger.info("TTS WAV clipped for ESP32: source_frames=%d kept=%d", source_frames, frames_to_read)
    pcm16 = _resample_pcm_s16le_mono(
        pcm,
        source_rate=source_rate,
        source_channels=source_channels,
        target_rate=_ESP32_TTS_SAMPLE_RATE,
    )
    if not pcm16:
        return b""
    return _pcm_to_wav(pcm16, sample_rate=_ESP32_TTS_SAMPLE_RATE)


async def _emit_esp32_tts_audio_chunks(emit_chunk, *, segment_no: int, segment_text: str, audio: dict[str, Any], audio_b64: str) -> None:
    for offset in range(0, len(audio_b64), _TTS_AUDIO_B64_CHUNK_CHARS):
        payload: dict[str, Any] = {
            "type": "tts_audio_chunk",
            "segment_no": segment_no,
            "audio_b64": audio_b64[offset : offset + _TTS_AUDIO_B64_CHUNK_CHARS],
        }
        for key in ("device_id", "session_id", "audio_id", "format", "voice"):
            value = audio.get(key)
            if value:
                payload[key] = value
        await emit_chunk(
            payload
        )
        await asyncio.sleep(_TTS_AUDIO_CHUNK_DELAY_SECONDS)


async def _emit_esp32_tts_pcm_chunk(
    emit_chunk,
    *,
    device_id: str,
    session_id: str,
    chunk_no: int,
    pcm: bytes,
) -> None:
    if not pcm:
        return
    await emit_chunk(
        {
            "type": "tts_pcm_chunk",
            "device_id": device_id,
            "session_id": session_id,
            "chunk_no": chunk_no,
            "audio_b64": base64.b64encode(pcm).decode("ascii"),
        }
    )


def _select_single_wav_from_base64(audio_b64: str) -> str:
    compact = re.sub(r"\s+", "", str(audio_b64 or ""))
    if not compact:
        return ""
    try:
        audio_bytes = base64.b64decode(compact, validate=False)
    except Exception:
        logger.warning("TTS base64 decode failed before ESP32 send")
        return ""
    riff_index = audio_bytes.rfind(b"RIFF")
    if riff_index > 0 and audio_bytes[riff_index + 8 : riff_index + 12] == b"WAVE":
        audio_bytes = audio_bytes[riff_index:]
    if audio_bytes[:4] != b"RIFF" or len(audio_bytes) < 44 or audio_bytes[8:12] != b"WAVE":
        logger.warning("TTS did not return a WAV payload: bytes=%d head=%r", len(audio_bytes), audio_bytes[:12])
        return ""
    wav_len = int.from_bytes(audio_bytes[4:8], "little", signed=False) + 8
    if wav_len < 44 or wav_len > len(audio_bytes):
        logger.warning("TTS WAV payload incomplete: declared=%d actual=%d", wav_len, len(audio_bytes))
        return ""
    audio_bytes = audio_bytes[:wav_len]
    offset = 12
    have_fmt = False
    data_len = 0
    sample_rate = 0
    channels = 0
    bits_per_sample = 0
    while offset + 8 <= len(audio_bytes):
        chunk_id = audio_bytes[offset : offset + 4]
        chunk_size = int.from_bytes(audio_bytes[offset + 4 : offset + 8], "little", signed=False)
        payload = offset + 8
        if payload + chunk_size > len(audio_bytes):
            logger.warning("TTS WAV chunk overflow: id=%r size=%d payload=%d total=%d", chunk_id, chunk_size, payload, len(audio_bytes))
            return ""
        if chunk_id == b"fmt ":
            if chunk_size < 16:
                logger.warning("TTS WAV fmt chunk too small: %d", chunk_size)
                return ""
            audio_format = int.from_bytes(audio_bytes[payload : payload + 2], "little", signed=False)
            channels = int.from_bytes(audio_bytes[payload + 2 : payload + 4], "little", signed=False)
            sample_rate = int.from_bytes(audio_bytes[payload + 4 : payload + 8], "little", signed=False)
            bits_per_sample = int.from_bytes(audio_bytes[payload + 14 : payload + 16], "little", signed=False)
            if audio_format != 1 or channels not in (1, 2) or bits_per_sample != 16:
                logger.warning(
                    "TTS WAV unsupported fmt: format=%d channels=%d rate=%d bits=%d",
                    audio_format,
                    channels,
                    sample_rate,
                    bits_per_sample,
                )
                return ""
            have_fmt = True
        elif chunk_id == b"data":
            if not have_fmt or chunk_size == 0:
                logger.warning("TTS WAV data chunk invalid: have_fmt=%s size=%d", have_fmt, chunk_size)
                return ""
            data_len = chunk_size
            break
        offset = payload + chunk_size + (chunk_size & 1)
    if not have_fmt or data_len == 0:
        logger.warning("TTS WAV missing fmt/data: bytes=%d head=%r", len(audio_bytes), audio_bytes[:32])
        return ""
    normalized = _normalize_wav_for_esp32(audio_bytes)
    if not normalized:
        return ""
    logger.info(
        "TTS WAV validated: source_bytes=%d source_rate=%d source_channels=%d bits=%d source_data=%d esp32_bytes=%d esp32_rate=%d",
        len(audio_bytes),
        sample_rate,
        channels,
        bits_per_sample,
        data_len,
        len(normalized),
        _ESP32_TTS_SAMPLE_RATE,
    )
    _save_tts_debug_wav(normalized)
    return base64.b64encode(normalized).decode("ascii")


def _save_tts_debug_wav(wav_bytes: bytes) -> None:
    try:
        _TTS_DEBUG_DIR.mkdir(parents=True, exist_ok=True)
        (_TTS_DEBUG_DIR / "last_tts.wav").write_bytes(wav_bytes)
        if os.getenv("MCP_TTS_DEBUG_HISTORY", "").strip().lower() in {"1", "true", "yes", "on"}:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
            (_TTS_DEBUG_DIR / f"tts_{timestamp}.wav").write_bytes(wav_bytes)
            keep = max(1, int(os.getenv("MCP_TTS_DEBUG_KEEP", "20")))
            history = sorted(_TTS_DEBUG_DIR.glob("tts_*.wav"), key=lambda path: path.stat().st_mtime, reverse=True)
            for old_path in history[keep:]:
                old_path.unlink(missing_ok=True)
    except Exception as exc:
        logger.warning("保存 TTS 调试 WAV 失败: %s", exc)


async def _send_single_tts_segment(tts_service: TTSModelService, emit_chunk, text: str) -> None:
    segment_text = _sanitize_spoken_text(text)
    if not segment_text:
        return
    segment_no = 0
    await emit_chunk({"type": "tts_segment_start", "segment_no": segment_no, "text": segment_text})
    try:
        async for audio in tts_service.stream_audio(segment_text):
            audio_b64 = _select_single_wav_from_base64(str(audio.get("audio_b64") or ""))
            if audio_b64:
                logger.info("TTS WAV ready for ESP32: b64=%d chars", len(audio_b64))
                await _emit_esp32_tts_audio_chunks(
                    emit_chunk,
                    segment_no=segment_no,
                    segment_text=segment_text,
                    audio=audio,
                    audio_b64=audio_b64,
                )
            else:
                await emit_chunk(
                    {
                        "type": "tts_error",
                        "segment_no": segment_no,
                        "text": segment_text,
                        "error": "TTS did not return a complete WAV payload",
                    }
                )
    except Exception as exc:
        logger.warning("语音合成输出失败: %s", exc)
        await emit_chunk(
            {
                "type": "tts_error",
                "segment_no": segment_no,
                "text": segment_text,
                "error": str(exc),
            }
        )
    finally:
        await emit_chunk({"type": "tts_stream_end"})


class StreamingTTSDispatcher:
    def __init__(
        self,
        tts_service: TTSModelService,
        emit_chunk,
        *,
        device_id: str = "",
        session_id: str = "",
        segment_stream_end: bool = False,
    ) -> None:
        self.tts_service = tts_service
        self.emit_chunk = emit_chunk
        self._queue: asyncio.Queue[tuple[int, str] | None] = asyncio.Queue(maxsize=4)
        self._buffer = ""
        self._segment_no = 0
        self._device_id = device_id
        self._session_id = session_id
        self._segment_stream_end = segment_stream_end
        self._sent_tts_status = False
        self.sent_audio_segments = 0
        self._worker = asyncio.create_task(self._run())

    async def push_text(self, text_chunk: str) -> None:
        self._buffer += text_chunk
        for segment in self._drain_segments(final=False):
            await self._queue.put((self._segment_no, segment))
            self._segment_no += 1

    async def finish(self) -> None:
        for segment in self._drain_segments(final=True):
            await self._queue.put((self._segment_no, segment))
            self._segment_no += 1
        await self._queue.put(None)
        await self._worker

    def _drain_segments(self, *, final: bool) -> list[str]:
        segments: list[str] = []
        while True:
            cut_index = -1
            for index, char in enumerate(self._buffer):
                if char in "。！？!?；;\n" and index >= _TTS_STRONG_PUNCT_MIN_CHARS:
                    cut_index = index
                    break
                if char in "，,、" and index >= _TTS_SOFT_PUNCT_MIN_CHARS:
                    cut_index = index
                    break
            if cut_index >= 0:
                segment = self._buffer[: cut_index + 1].strip()
                self._buffer = self._buffer[cut_index + 1 :]
                if segment:
                    segments.append(segment)
                continue
            if not final and len(self._buffer) >= _TTS_MAX_SEGMENT_CHARS:
                segment = self._buffer[:_TTS_MAX_SEGMENT_CHARS].strip()
                self._buffer = self._buffer[_TTS_MAX_SEGMENT_CHARS:]
                if segment:
                    segments.append(segment)
                continue
            break
        if final and self._buffer.strip():
            segments.append(self._buffer.strip())
            self._buffer = ""
        return segments

    async def force_flush(self) -> None:
        for segment in self._drain_segments(final=True):
            await self._queue.put((self._segment_no, segment))
            self._segment_no += 1

    async def _run(self) -> None:
        while True:
            item = await self._queue.get()
            if item is None:
                if not self._segment_stream_end:
                    await self._emit_tts_event({"type": "tts_stream_end"})
                return
            segment_no, segment_text = item
            segment_text = _sanitize_spoken_text(segment_text)
            if not segment_text:
                continue
            segment_ok = False
            if self._segment_stream_end and not self._sent_tts_status:
                await self._emit_tts_event({"type": "assistant_status", "status": "tts", "text": segment_text[:120]})
                self._sent_tts_status = True
            await self._emit_tts_event({"type": "tts_segment_start", "segment_no": segment_no, "text": segment_text})
            try:
                async for audio in self.tts_service.stream_audio(segment_text):
                    audio_b64 = _select_single_wav_from_base64(str(audio.get("audio_b64") or ""))
                    if audio_b64:
                        await _emit_esp32_tts_audio_chunks(
                            self._emit_tts_event,
                            segment_no=segment_no,
                            segment_text=segment_text,
                            audio=audio,
                            audio_b64=audio_b64,
                        )
                        segment_ok = True
                        self.sent_audio_segments += 1
                    else:
                        payload = {
                            "type": "tts_error",
                            "segment_no": segment_no,
                            "text": segment_text,
                            "error": "TTS did not return a complete WAV payload",
                        }
                        await self._emit_tts_event(payload)
            except Exception as exc:
                logger.warning("语音合成流式输出失败: %s", exc)
                await self._emit_tts_event(
                    {
                        "type": "tts_error",
                        "segment_no": segment_no,
                        "text": segment_text,
                        "error": str(exc),
                    }
                )
            finally:
                if self._segment_stream_end and segment_ok:
                    await self._emit_tts_event({"type": "tts_stream_end", "segment_no": segment_no})

    async def _emit_tts_event(self, payload: dict[str, Any]) -> None:
        if self._device_id:
            payload.setdefault("device_id", self._device_id)
        if self._session_id:
            payload.setdefault("session_id", self._session_id)
        await self.emit_chunk(payload)


class RobotRuntime:
    def __init__(self, config: AppConfig, connection_manager: ConnectionManagerProtocol) -> None:
        self.config = config
        self.connection_manager = connection_manager
        self.language_model = LanguageModelService(config.language_model)
        self.tool_model = ToolModelService(config.tool_model)
        self.context_embedding = ContextEmbeddingService(config.context_embedding) if config.context_embedding.api_key else None
        self.vision_model = VisionModelService(config.vision_model)
        self.vision_highres_model = VisionModelService(config.vision_highres_model)
        self.asr_model = ASRModelService(config.asr_model, language=config.asr_language) if config.asr_model.api_key else None
        self.doubao_dialog = DoubaoRealtimeDialogClient(config.doubao_dialog)
        self.tts_model = (
            TTSModelService(config.tts_model, config.tts_voice, config.tts_style_prompt)
            if config.tts_model.api_key
            else None
        )
        self.memory_store = SubconsciousMemoryStore(
            config.subconscious_file,
            half_life_hours=config.subconscious_half_life_hours,
        )
        self.structured_memory_store = StructuredMemoryStore(
            config.data_dir / "structured_memory.jsonl",
            config.data_dir / "user_profiles.jsonl",
        )
        self.frame_store = FrameStore(config.data_dir / "frames")
        self.conversation_store = ConversationStore(
            config.data_dir / "conversation_history.jsonl",
            cache_limit=config.conversation_cache_size,
        )
        self.agent_bridge = GenericAgentBridge(config.generic_agent_root, config.generic_agent_python)
        self.tool_registry = self._build_tool_registry()
        self.task_manager = BackgroundTaskManager()
        self.approval_manager = ApprovalManager()
        self._tasks: set[asyncio.Task[Any]] = set()
        self._memory_queue: asyncio.Queue[tuple[str, str] | None] | None = None
        self._memory_worker: asyncio.Task[Any] | None = None
        self._queued_memory_sessions: set[str] = set()
        self._pending_image_jobs: dict[str, PendingImageJob] = {}
        self._pending_audio_streams: dict[str, PendingAudioStream] = {}
        self._esp32_runtime_config: dict[str, ESP32RuntimeConfig] = {}
        self._voice_presets_cache: dict[str, str] | None = None
        self._voice_presets_mtime: float = 0.0
        self._offline_gap_analysis_keys: set[str] = set()
        self.audio_capture_dir = self.config.data_dir / "esp32_audio"
        self.audio_capture_dir.mkdir(parents=True, exist_ok=True)
        self.config.doubao_dialog.persona_dir.mkdir(parents=True, exist_ok=True)

    def _cleanup_stale_audio_streams(self) -> None:
        if not self._pending_audio_streams:
            return
        now = time.time()
        expired: list[str] = []
        for session_id, stream in self._pending_audio_streams.items():
            if now - stream.updated_at <= 90:
                continue
            expired.append(session_id)
            try:
                if stream.pcm_path.exists():
                    stream.pcm_path.unlink()
            except Exception:
                logger.warning("删除过期 PCM 临时文件失败: %s", stream.pcm_path)
        for session_id in expired:
            stream = self._pending_audio_streams.pop(session_id, None)
            if stream is None:
                continue
            self._trace(
                "audio.capture.error",
                source="ESP32",
                device_id=stream.device_id,
                session_id=session_id,
                error="audio stream expired before completion",
            )

    def _build_tool_registry(self) -> ToolRegistry:
        registry = ToolRegistry()
        registry.register(SeniverseWeatherTool(self.config.tool_api.seniverse_key))
        registry.register(
            DashScopeQuarkSearchTool(
                api_key=self.config.tool_api.quark_search_api_key,
                agent_id=self.config.tool_api.quark_search_agent_id,
                agent_version=self.config.tool_api.quark_search_agent_version,
                workspace_id=self.config.tool_api.quark_search_workspace_id,
            )
        )
        registry.register(AmapTool(self.config.tool_api.amap_key))
        registry.register(HighResVisionTool(self.frame_store, self.vision_highres_model))
        return registry

    async def send_esp32_tts_test(self, text: str = "小乐测试语音。") -> None:
        if self.tts_model is None:
            raise RuntimeError("TTS model not configured")
        esp32_ready = bool(getattr(self.connection_manager, "is_esp32_connected", True))
        if not esp32_ready:
            raise RuntimeError("ESP32 is not connected")
        await _send_single_tts_segment(self.tts_model, self.connection_manager.send_to_esp32, text)
        await self.connection_manager.send_to_esp32(
            {
                "type": "assistant_done",
                "device_id": "ESP32_KORVO_2",
                "text": text,
            }
        )
        await _send_esp32_status(
            self.connection_manager,
            status="idle",
            device_id="ESP32_KORVO_2",
            text=text,
        )

    def _trace(self, event: str, **payload: Any) -> None:
        body = {"event": event, **payload}
        try:
            trace_logger.info(json.dumps(body, ensure_ascii=False, default=str))
        except Exception:
            trace_logger.info("%s | %s", event, payload)

    @staticmethod
    def _parse_iso_timestamp(raw: str) -> datetime | None:
        text = str(raw or "").strip()
        if not text:
            return None
        try:
            return datetime.fromisoformat(text)
        except Exception:
            return None

    def _prepare_offline_gap_analysis(
        self,
        *,
        session_id: str,
        source: str,
        return_user_text: str,
    ) -> OfflineGapAnalysisCandidate | None:
        if not self.config.offline_gap_analysis_enabled:
            return None
        latest = self.conversation_store.latest_entry(session_id=session_id)
        if not latest:
            return None
        last_turn_timestamp = str(latest.get("timestamp") or "")
        last_dt = self._parse_iso_timestamp(last_turn_timestamp)
        if last_dt is None:
            return None
        now_dt = datetime.now().astimezone()
        gap_minutes = max(0.0, (now_dt - last_dt).total_seconds() / 60.0)
        if gap_minutes < float(self.config.offline_gap_threshold_minutes):
            return None
        dedup_key = f"{session_id}|{last_turn_timestamp}|{return_user_text.strip()}"
        if dedup_key in self._offline_gap_analysis_keys:
            return None
        self._offline_gap_analysis_keys.add(dedup_key)
        return OfflineGapAnalysisCandidate(
            session_id=session_id,
            source=source,
            gap_minutes=gap_minutes,
            last_turn_timestamp=last_turn_timestamp,
            now_timestamp=now_dt.isoformat(),
            pre_gap_entries=self.conversation_store.recent_entries(
                session_id=session_id,
                limit=self.config.offline_gap_recent_entries,
            ),
            pre_gap_summaries=self.conversation_store.recent_summaries(session_id=session_id, limit=3),
            return_user_text=return_user_text.strip(),
        )

    @staticmethod
    def _format_gap_label(gap_minutes: float) -> str:
        total_minutes = max(1, int(round(gap_minutes)))
        if total_minutes < 60:
            return f"{total_minutes} 分钟"
        hours, minutes = divmod(total_minutes, 60)
        if hours < 24:
            return f"{hours} 小时 {minutes} 分钟" if minutes else f"{hours} 小时"
        days, hours = divmod(hours, 24)
        if hours:
            return f"{days} 天 {hours} 小时"
        return f"{days} 天"

    async def _build_temporal_return_context(
        self,
        *,
        session_id: str,
        source: str,
        request_text: str,
    ) -> tuple[str, float]:
        if not self.config.offline_gap_analysis_enabled:
            return "", 0.0
        latest = self.conversation_store.latest_entry(session_id=session_id)
        if not latest:
            return "", 0.0
        last_turn_timestamp = str(latest.get("timestamp") or "").strip()
        last_dt = self._parse_iso_timestamp(last_turn_timestamp)
        if last_dt is None:
            return "", 0.0
        now_dt = datetime.now().astimezone()
        gap_minutes = max(0.0, (now_dt - last_dt).total_seconds() / 60.0)
        if gap_minutes < float(self.config.offline_gap_threshold_minutes):
            return "", gap_minutes

        profile_facts = [
            item.summary
            for item in self.structured_memory_store.profile_facts(scope_id=session_id, limit=6)
            if item.summary
        ]
        active_memories = [
            f"[{item.memory_type}] {item.text}"
            for item in self.structured_memory_store.active_memories(session_id=session_id, limit=6)
            if item.text
        ]
        recent_entries = self.conversation_store.recent_entries(
            session_id=session_id,
            limit=self.config.offline_gap_recent_entries,
        )
        recent_summaries = self.conversation_store.recent_summaries(session_id=session_id, limit=3)

        try:
            result = await asyncio.wait_for(
                self.tool_model.build_returning_user_context(
                    session_id=session_id,
                    source=source,
                    gap_minutes=gap_minutes,
                    last_turn_timestamp=last_turn_timestamp,
                    now_timestamp=now_dt.isoformat(),
                    current_user_text=request_text,
                    recent_entries=recent_entries,
                    recent_summaries=recent_summaries,
                    profile_facts=profile_facts,
                    active_memories=active_memories,
                ),
                timeout=18.0,
            )
        except Exception as exc:
            logger.warning("首轮时间上下文注入失败: %s", exc)
            self._trace(
                "context.temporal_gap.error",
                session_id=session_id,
                source=source,
                gap_minutes=round(gap_minutes, 2),
                error=str(exc),
            )
            gap_label = self._format_gap_label(gap_minutes)
            fallback_lines = [
                "时间感知回归上下文:",
                f"- 距上一轮约 {gap_label}，当前消息应视为断档后的重新校准信号。",
                f"- 上次交互时间: {last_turn_timestamp}",
                f"- 当前时间: {now_dt.isoformat()}",
            ]
            if profile_facts:
                fallback_lines.append(f"- 稳定用户画像: {'；'.join(profile_facts[:3])}")
            if active_memories:
                fallback_lines.append(f"- 结构化记忆: {'；'.join(active_memories[:3])}")
            fallback_lines.append("- 回复前先考虑用户场景可能变化，再结合历史补足稳定背景。")
            prompt_block = "\n".join(fallback_lines)
            self._trace(
                "context.temporal_gap.injected",
                session_id=session_id,
                source=source,
                gap_minutes=round(gap_minutes, 2),
                confidence=0.0,
                mode="fallback",
                profile_count=len(profile_facts),
                memory_count=len(active_memories),
            )
            logger.info("首轮时间上下文已注入(兜底): 会话=%s 间隔=%.1f分钟", session_id, gap_minutes)
            return prompt_block, gap_minutes

        prompt_block = str(result.get("prompt_block") or "").strip()
        if not prompt_block:
            prompt_parts = [
                str(result.get("time_consideration") or "").strip(),
                str(result.get("reply_strategy") or "").strip(),
            ]
            prompt_block = "\n".join(f"- {part}" for part in prompt_parts if part)
        if not prompt_block:
            return "", gap_minutes
        temporal_context = "\n".join(
            [
                "时间感知回归上下文:",
                f"- 当前时间: {now_dt.isoformat()}",
                f"- 上次交互时间: {last_turn_timestamp}",
                f"- 时间间隔: {self._format_gap_label(gap_minutes)}",
                prompt_block,
            ]
        ).strip()
        self._trace(
            "context.temporal_gap.injected",
            session_id=session_id,
            source=source,
            gap_minutes=round(gap_minutes, 2),
            confidence=round(float(result.get('confidence') or 0.0), 3),
            mode="model",
            profile_count=len(profile_facts),
            memory_count=len(active_memories),
            log_summary=str(result.get("log_summary") or "").strip(),
        )
        logger.info(
            "首轮时间上下文已注入: 会话=%s 间隔=%.1f分钟 置信度=%.2f",
            session_id,
            gap_minutes,
            float(result.get("confidence") or 0.0),
        )
        return temporal_context, gap_minutes

    async def _run_offline_gap_analysis(
        self,
        *,
        candidate: OfflineGapAnalysisCandidate,
        return_assistant_text: str,
    ) -> None:
        self._trace(
            "temporal.offline_gap.start",
            session_id=candidate.session_id,
            source=candidate.source,
            gap_minutes=round(candidate.gap_minutes, 2),
            text=candidate.return_user_text,
        )
        try:
            result = await self.tool_model.analyze_offline_gap(
                session_id=candidate.session_id,
                source=candidate.source,
                gap_minutes=candidate.gap_minutes,
                pre_gap_entries=candidate.pre_gap_entries,
                pre_gap_summaries=candidate.pre_gap_summaries,
                return_user_text=candidate.return_user_text,
                return_assistant_text=return_assistant_text,
                last_turn_timestamp=candidate.last_turn_timestamp,
                now_timestamp=candidate.now_timestamp,
            )
        except Exception as exc:
            self._trace(
                "temporal.offline_gap.error",
                session_id=candidate.session_id,
                source=candidate.source,
                gap_minutes=round(candidate.gap_minutes, 2),
                error=str(exc),
            )
            return
        self._trace(
            "temporal.offline_gap.done",
            session_id=candidate.session_id,
            source=candidate.source,
            gap_minutes=round(candidate.gap_minutes, 2),
            confidence=round(float(result.get("confidence") or 0.0), 3),
            pre_gap_state=result.get("pre_gap_state") or "",
            return_fit=result.get("return_fit") or "",
            prompt_preview=result.get("prompt_injection_preview") or "",
            offline_activities=result.get("likely_offline_activities") or [],
            new_constraints=result.get("likely_new_constraints") or [],
            summary=result.get("log_summary") or "",
        )

    def health_snapshot(self) -> dict[str, Any]:
        return {
            "shared_session_id": self.config.shared_session_id,
            "tool_count": len(self.tool_registry.catalog()),
            "active_background_tasks": len([task for task in self._tasks if not task.done()]),
            "memory_worker_running": self._memory_worker is not None and not self._memory_worker.done(),
            "models": {
                "language": self.language_model.model_name,
                "tool": self.tool_model.model_name,
                "vision_low": self.vision_model.model_name,
                "vision_high": self.vision_highres_model.model_name,
                "asr": self.asr_model.model_name if self.asr_model else "",
                "tts": self.tts_model.model_name if self.tts_model else "",
                "embedding": self.context_embedding.model_name if self.context_embedding else "",
            },
        }

    def shared_context_snapshot(self) -> dict[str, Any]:
        session_id = self.config.shared_session_id
        context = self.conversation_store.export_context_snapshot(
            session_id=session_id,
            recent_limit=self.config.context_recent_turns,
            retrieval_limit=self.config.context_rag_hits,
        )
        context["latest_frame"] = self.frame_store.latest_payload(device_id=session_id)
        context["subconscious_context"] = self.memory_store.build_context()
        return context

    def latest_frame_snapshot(self) -> dict[str, Any] | None:
        return self.frame_store.latest_payload(device_id=self.config.shared_session_id) or self.frame_store.latest_payload()

    def subconscious_snapshot(self, *, limit: int = 12) -> dict[str, Any]:
        return {
            "context_text": self.memory_store.build_context(limit=limit),
            "items": self.memory_store.scored_memories(limit=limit),
        }

    async def recent_task_snapshot(self, *, limit: int = 20) -> list[dict[str, Any]]:
        return await self.task_manager.snapshot(limit=limit)

    async def pending_approval_snapshot(self, *, source: str | None = None, user_id: str = "") -> list[dict[str, Any]]:
        return await self.approval_manager.list_pending(source=source, user_id=user_id)

    async def long_term_memory_snapshot(self, *, query: str, limit: int = 6) -> dict[str, Any]:
        session_id = self.config.shared_session_id
        query_embedding: list[float] | None = None
        if self.context_embedding is not None and self.context_embedding.enabled and query.strip():
            try:
                embedding_result = await self.context_embedding.embed_text(query)
                query_embedding = list(embedding_result.get("embedding") or [])
            except Exception as exc:
                logger.warning("长期记忆向量生成失败: %s", exc)
        return {
            "session_id": session_id,
            "query": query,
            "hits": self.conversation_store.retrieve_memory(
                session_id=session_id,
                query=query,
                limit=limit,
                query_embedding=query_embedding,
            ),
        }

    async def drain_background_tasks(self, *, timeout_seconds: float = 10.0) -> None:
        deadline = time.perf_counter() + max(0.1, timeout_seconds)
        while time.perf_counter() < deadline:
            pending = [
                task
                for task in self._tasks
                if not task.done() and task is not self._memory_worker
            ]
            if not pending:
                return
            await asyncio.wait(pending, timeout=min(0.5, max(0.05, deadline - time.perf_counter())))

    def _is_fast_chat_candidate(self, text: str) -> bool:
        stripped = text.strip().lower()
        if not stripped or len(stripped) > 48 or "\n" in stripped:
            return False
        tool_markers = (
            "天气",
            "weather",
            "搜索",
            "搜一下",
            "搜搜",
            "search",
            "查一下",
            "查查",
            "导航",
            "地图",
            "高德",
            "定位",
            "路线",
            "怎么去",
            "怎么走",
            "夸克",
            "百炼",
            "心知",
            "联网",
            "上网",
            "新闻",
            "百科",
            "资料",
            "图片",
            "视觉",
            "拍照",
            "后台",
            "文件",
            "进程",
            "root/",
            "change/",
            "change_model:",
            "change_model/",
            "exchange/",
            "exchange:",
            "exchange_model:",
            "exchange_model/",
        )
        return not any(marker in stripped for marker in tool_markers)

    def _detect_fast_tool_route(self, text: str) -> SemanticPlan | None:
        stripped = text.strip()
        lowered = stripped.lower()
        impaired_start = lowered.startswith(
            (
                "root/",
                "change/",
                "change_model:",
                "change_model/",
                "exchange/",
                "exchange:",
                "exchange_model:",
                "exchange_model/",
            )
        )
        if not stripped or len(stripped) > 120 or "\n" in stripped or impaired_start:
            return None
        if any(marker in stripped for marker in ("顺便", "然后", "同时", "并且", "另外", "以及", "还有")):
            return None

        weather_markers = (
            "天气",
            "气温",
            "温度",
            "降雨",
            "下雨",
            "预报",
            "weather",
            "forecast",
        )
        navigation_markers = (
            "导航",
            "路线",
            "路程",
            "地图",
            "高德",
            "地址",
            "经纬度",
            "坐标",
            "怎么去",
            "怎么走",
            "route",
            "map",
        )
        search_markers = (
            "搜索",
            "搜一下",
            "搜搜",
            "查一下",
            "查查",
            "联网",
            "上网",
            "夸克",
            "百炼",
            "百科",
            "新闻",
            "资料",
            "search",
        )

        if any(marker in lowered for marker in weather_markers):
            return SemanticPlan(
                intent="weather",
                needs_tools=True,
                should_reply=True,
                tool_goal=stripped,
                response_style="先给出天气结果，再用简短自然的话总结重点。",
            )
        if any(marker in lowered for marker in navigation_markers):
            return SemanticPlan(
                intent="navigation",
                needs_tools=True,
                should_reply=True,
                tool_goal=stripped,
                response_style="先给出路线或定位结果，再简洁说明下一步。",
            )
        if any(marker in lowered for marker in search_markers):
            return SemanticPlan(
                intent="search",
                needs_tools=True,
                should_reply=True,
                tool_goal=stripped,
                response_style="先给出检索结果，再简洁归纳关键信息。",
        )
        return None

    def _extract_change_command(self, text: str) -> str | None:
        normalized = text.strip()
        lowered = normalized.lower()
        prefixes = (
            "change/",
            "change_model:",
            "change_model/",
            "exchange/",
            "exchange:",
            "exchange_model:",
            "exchange_model/",
        )
        for prefix in prefixes:
            if lowered.startswith(prefix):
                return normalized[len(prefix) :].strip()
        return None

    def _should_summarize_root_result(self, text: str) -> bool:
        if not text.strip():
            return True
        lowered = text.lower()
        noisy_markers = (
            "llm running",
            "tool_use",
            "code run output",
            "🛠️",
            "[debug]",
            "[cache]",
            "full prompt length",
            "<tool",
        )
        return any(marker in lowered for marker in noisy_markers)

    def _should_build_qq_summary(self, text: str) -> bool:
        stripped = text.strip()
        if not stripped:
            return False
        if len(stripped) <= self.config.qq_summary_max_chars and stripped.count("\n") < 2:
            return False
        return True

    def _image_job_key(self, request: TextRequest) -> str:
        if request.message_type == "group" and request.group_id:
            return f"napcat:group:{request.group_id}"
        return f"napcat:user:{request.user_id or 'unknown'}"

    def _prune_pending_image_jobs(self, *, ttl_seconds: float = 300.0) -> None:
        now = time.time()
        stale_keys = [key for key, job in self._pending_image_jobs.items() if now - job.updated_at > ttl_seconds]
        for key in stale_keys:
            self._pending_image_jobs.pop(key, None)

    def _register_pending_image_job(self, request: TextRequest, *, session_id: str, frame_id: str) -> PendingImageJob:
        self._prune_pending_image_jobs()
        key = self._image_job_key(request)
        job = PendingImageJob(
            chat_key=key,
            session_id=session_id,
            source=request.source,
            message_type=request.message_type,
            frame_id=frame_id,
            user_id=request.user_id,
            group_id=request.group_id,
        )
        self._pending_image_jobs[key] = job
        return job

    def _get_pending_image_job(self, request: TextRequest) -> PendingImageJob | None:
        self._prune_pending_image_jobs()
        return self._pending_image_jobs.get(self._image_job_key(request))

    def _looks_like_image_detail_query(self, text: str) -> bool:
        normalized = re.sub(r"\s+", "", (text or "").strip().lower())
        if not normalized:
            return False
        direct_markers = (
            "图片",
            "照片",
            "图里",
            "图中",
            "图上",
            "画里",
            "画面",
            "截图",
            "刚刚发的",
            "刚才发的",
            "上一张",
            "上个图",
            "这张图",
            "这个图",
            "这幅图",
            "刚才那张",
        )
        if any(marker in normalized for marker in direct_markers):
            return True
        detail_markers = (
            "这是什么",
            "这是啥",
            "这是谁",
            "这个是谁",
            "叫什么",
            "叫啥",
            "哪部",
            "哪个角色",
            "什么角色",
            "看清",
            "认出来",
            "认得出来",
            "细节",
            "写了什么",
            "上面写",
        )
        if any(marker in normalized for marker in detail_markers):
            return True
        if normalized.startswith(("这张", "这个", "这是", "这是不是", "刚发的", "刚刚那个", "刚才那个")) and any(
            token in normalized for token in ("谁", "什么", "哪", "吗", "？", "?", "细节", "写")
        ):
            return True
        return False

    def _looks_like_image_preface(self, text: str) -> bool:
        normalized = re.sub(r"\s+", "", (text or "").strip().lower())
        if not normalized or len(normalized) > 60:
            return False
        markers = (
            "给你看",
            "给你发",
            "你看这个",
            "你看这个",
            "来看看",
            "看这个",
            "看下这个",
            "给你瞅瞅",
            "给你看看",
            "我发你个",
            "我给你发个",
        )
        if not any(marker in normalized for marker in markers):
            return False
        blocking = (
            "天气",
            "搜索",
            "查一下",
            "导航",
            "地图",
            "怎么去",
            "root/",
            "change/",
        )
        return not any(marker in normalized for marker in blocking)

    def _rewrite_image_summary_naturally(self, *, user_text: str, memory_summary: str) -> str:
        summary = re.sub(r"\s+", " ", (memory_summary or "").strip())
        if not summary:
            return ""
        summary = re.sub(r"^(这张|这个|该)?图片(展示了|里是|内容是|主要是)?[:：，,\s]*", "", summary)
        summary = re.sub(r"^(手机屏幕显示|界面显示|画面显示|画面中|画面里|图中显示|图里是)[:：，,\s]*", "", summary)
        parts = [part.strip(" ，,。；;") for part in re.split(r"[；;]", summary) if part.strip(" ，,。；;")]
        summary = parts[0] if parts else summary.strip(" ，,。；;")
        sentence_parts = [part.strip() for part in re.split(r"(?<=[。！？!?])", summary) if part.strip()]
        summary = sentence_parts[0] if sentence_parts else summary
        summary = summary.strip(" ，,。；;")
        if not summary:
            return ""
        if len(summary) > 92:
            summary = summary[:92].rstrip(" ，,；;")
        if not re.search(r"(像是|看起来|大概率|应该是|像)", summary):
            summary = f"看起来像是{summary}"
        if user_text and any(token in user_text for token in ("好看", "漂亮", "可爱", "帅")):
            prefix = "噢~我看到了，"
        elif user_text and any(token in user_text for token in ("好吃", "香", "辣", "午餐", "饭", "菜")):
            prefix = "噢~我看到了，"
        else:
            prefix = "噢~我知道了，"
        return f"{prefix}{summary}。"

    def _build_image_preface_ack(self, text: str) -> str:
        normalized = (text or "").strip()
        if "漫画" in normalized:
            return "好滴，你发来我就看，我先接住这张漫画。"
        if "截图" in normalized:
            return "好呀，你发过来吧，我帮你看看这张截图。"
        if "聊天记录" in normalized:
            return "好滴，发来吧，我看看这段聊天记录。"
        if any(token in normalized for token in ("午餐", "饭", "吃的", "菜")):
            return "好呀，发来我看看，顺便帮你判断一下看起来香不香。"
        return "好滴，你发来吧，我接着看。"

    def _find_recent_image_preface(self, request: TextRequest, *, limit: int = 12, max_age_seconds: float = 600.0) -> str:
        session_id = self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id, device_id=request.device_id)
        entries = self.conversation_store.recent_entries(session_id=session_id, limit=limit)
        now = datetime.now(timezone.utc)
        for entry in reversed(entries):
            if str(entry.get("source") or "") != request.source:
                continue
            user_text = str(entry.get("user_text") or "").strip()
            if not self._looks_like_image_preface(user_text):
                continue
            try:
                ts = datetime.fromisoformat(str(entry.get("timestamp") or ""))
                if ts.tzinfo is None:
                    ts = ts.replace(tzinfo=timezone.utc)
                age_seconds = max(0.0, (now - ts.astimezone(timezone.utc)).total_seconds())
                if age_seconds > max_age_seconds:
                    continue
            except Exception:
                pass
            return user_text
        return ""

    def _find_recent_image_context_query(self, request: TextRequest, *, limit: int = 10) -> str:
        session_id = self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id, device_id=request.device_id)
        entries = self.conversation_store.recent_entries(session_id=session_id, limit=limit)
        for entry in reversed(entries):
            if str(entry.get("source") or "") != request.source:
                continue
            if str(entry.get("kind") or "") != "turn":
                continue
            user_text = str(entry.get("user_text") or "").strip()
            if not user_text:
                continue
            if self._looks_like_image_preface(user_text):
                continue
            normalized = re.sub(r"\s+", "", user_text)
            if len(normalized) <= 1:
                continue
            return user_text
        return ""

    async def _compose_natural_image_followup(self, *, user_text: str, memory_summary: str) -> str:
        text = await self.language_model.compose_image_followup(
            user_text=user_text,
            memory_summary=memory_summary,
        )
        if self._looks_like_raw_image_followup(text, memory_summary):
            polished = await self.tool_model.polish_image_followup(
                user_text=user_text,
                memory_summary=memory_summary,
            )
            if polished and not self._looks_like_raw_image_followup(polished, memory_summary):
                text = polished
        cleaned = text.strip()
        if cleaned and not self._looks_like_raw_image_followup(cleaned, memory_summary):
            return cleaned
        rewritten = self._rewrite_image_summary_naturally(user_text=user_text, memory_summary=memory_summary)
        if rewritten:
            return rewritten
        return f"噢~我知道了，看起来这张图里像是：{memory_summary}"

    def _mark_pending_image_detail_request(self, request: TextRequest, query_text: str) -> tuple[PendingImageJob | None, str]:
        job = self._get_pending_image_job(request)
        if job is None or job.status == "completed":
            return None, ""
        job.latest_query = query_text.strip()
        job.waiting_for_followup = True
        job.updated_at = time.time()
        if job.hold_message_sent:
            return job, "我还在看这张图，再等我一下，马上补给你。"
        job.hold_message_sent = True
        return job, "让我先看看嘛，这张图我还在补细节，等我看清了马上告诉你。"

    def _looks_like_referential_followup(self, text: str) -> bool:
        normalized = re.sub(r"\s+", "", (text or "").strip().lower())
        if not normalized:
            return False
        if len(normalized) > 32:
            return False
        direct_markers = (
            "这个",
            "这张",
            "这幅",
            "它",
            "这个漫画",
            "这个角色",
            "这个人",
            "这个饭",
            "这顿",
            "这份",
            "这个菜",
            "刚发的",
            "刚刚那个",
            "刚才那个",
            "上面那个",
        )
        if any(marker in normalized for marker in direct_markers):
            return True
        followup_patterns = (
            "看过",
            "认识",
            "认得",
            "见过",
            "像不像",
            "是不是",
            "好看不",
            "好看吗",
            "漂亮吗",
            "美吗",
            "可爱吗",
            "可不可爱",
            "帅吗",
            "好不好吃",
            "好吃吗",
            "怎么样",
            "咋样",
            "香吗",
            "辣吗",
            "能吃吗",
            "是谁",
            "什么",
        )
        if any(marker in normalized for marker in followup_patterns):
            return True
        if normalized.startswith(("这", "那")) and any(
            marker in normalized
            for marker in (
                "好",
                "像",
                "是",
                "吗",
                "不",
                "咋样",
                "怎么样",
                "认得",
                "认识",
                "看过",
            )
        ):
            return True
        return False

    def _get_recent_frame_for_request(self, request: TextRequest, *, max_age_seconds: float = 1800.0):
        session_id = self._session_id_for(
            request.source,
            user_id=request.user_id,
            group_id=request.group_id,
            device_id=request.device_id,
        )
        matches = self.frame_store.list_recent(device_id=session_id, limit=1)
        if not matches:
            return None
        frame = matches[0]
        try:
            ts = datetime.fromisoformat(frame.timestamp)
            if ts.tzinfo is None:
                ts = ts.replace(tzinfo=timezone.utc)
            age_seconds = max(0.0, (datetime.now(timezone.utc) - ts.astimezone(timezone.utc)).total_seconds())
        except Exception:
            age_seconds = max_age_seconds + 1
        if age_seconds > max_age_seconds:
            return None
        return frame

    def _build_recent_frame_focus(self, frame) -> str:
        summary = frame.knowledge_summary or frame.highres_summary or frame.lowres_summary or frame.note or "最近收到过一张图片。"
        return (
            "当前这句大概率是在承接最近那张图片，请优先把它当成主要信息源来理解。\n"
            f"最近图片线索: id={frame.frame_id} time={frame.timestamp} summary={summary}"
        )

    async def _assemble_request_context(self, request: TextRequest, request_text: str) -> RequestContextBundle:
        session_id = self._session_id_for(
            request.source,
            user_id=request.user_id,
            group_id=request.group_id,
            device_id=request.device_id,
        )
        recent_frame = self._get_recent_frame_for_request(request)
        referential_followup = recent_frame is not None and self._looks_like_referential_followup(request_text)
        realtime_esp32 = request.source == "ESP32" and self.config.esp32_realtime_mode
        if realtime_esp32 and self.config.esp32_realtime_skip_temporal_context:
            temporal_context, temporal_gap_minutes = "", 0.0
        else:
            temporal_context, temporal_gap_minutes = await self._build_temporal_return_context(
                session_id=session_id,
                source=request.source,
                request_text=request_text,
            )
        subconscious_context = self.memory_store.build_context()
        structured_context = self.structured_memory_store.build_context(
            session_id=session_id,
            profile_scope=session_id,
        )
        query_embedding: list[float] | None = None
        if (
            self.context_embedding is not None
            and self.context_embedding.enabled
            and not (realtime_esp32 and self.config.esp32_realtime_skip_semantic_rag)
            and self.conversation_store.should_use_semantic_rag(request_text)
            and self.conversation_store.has_embeddings(session_id=session_id)
        ):
            try:
                embedding_result = await self.context_embedding.embed_text(request_text)
                query_embedding = list(embedding_result.get("embedding") or [])
                self._trace(
                    "context.embedding.query",
                    session_id=session_id,
                    source=request.source,
                    request_text=request_text,
                    dims=len(query_embedding),
                    request_id=embedding_result.get("request_id"),
                    usage=embedding_result.get("usage"),
                )
            except Exception as exc:
                logger.warning("上下文检索向量生成失败: %s", exc)
                self._trace(
                    "context.embedding.query.error",
                    session_id=session_id,
                    source=request.source,
                    request_text=request_text,
                    error=str(exc),
                )
        conversation_context = self.conversation_store.build_context(
            session_id=session_id,
            query=request_text,
            query_embedding=query_embedding,
            recent_limit=self.config.context_recent_turns,
            retrieval_limit=self.config.context_rag_hits,
        )
        latest_frame_context = self.frame_store.summary_context_for(request.device_id or session_id)
        if referential_followup and recent_frame is not None:
            latest_frame_context = self._build_recent_frame_focus(recent_frame)
        if structured_context:
            subconscious_context = f"{structured_context}\n{subconscious_context}".strip()
        if latest_frame_context:
            subconscious_context = f"{latest_frame_context}\n{subconscious_context}".strip()
        if conversation_context:
            subconscious_context = f"{conversation_context}\n{subconscious_context}".strip()
        if temporal_context:
            subconscious_context = f"{temporal_context}\n{subconscious_context}".strip()
        return RequestContextBundle(
            session_id=session_id,
            conversation_context=conversation_context,
            latest_frame_context=latest_frame_context,
            subconscious_context=subconscious_context,
            temporal_context=temporal_context,
            temporal_gap_minutes=temporal_gap_minutes,
            recent_frame=recent_frame,
            referential_followup=referential_followup,
        )

    async def _score_image_reply_relevance(
        self,
        *,
        request: TextRequest,
        request_text: str,
        image_summary: str,
        context_bundle: RequestContextBundle,
        threshold: int | None = None,
    ) -> tuple[bool, bool]:
        threshold = self.config.image_reply_relevance_threshold if threshold is None else threshold
        decision = await self.language_model.assess_image_reply_relevance(
            source=request.source,
            user_text=request_text,
            image_summary=image_summary,
            conversation_context=context_bundle.conversation_context,
            subconscious=context_bundle.subconscious_context,
            score_threshold=threshold,
        )
        explicit_image_query = self._looks_like_image_detail_query(request_text) or context_bundle.referential_followup
        should_reply_with_image = explicit_image_query or decision.should_reply
        should_fallback_text_only = not should_reply_with_image and not explicit_image_query
        self._trace(
            "vision.reply.relevance",
            source=request.source,
            user_id=request.user_id,
            group_id=request.group_id,
            device_id=request.device_id,
            model=self.language_model.model_name,
            score=decision.score,
            threshold=threshold,
            should_reply=should_reply_with_image,
            fallback_text_only=should_fallback_text_only,
            forced_by_explicit_query=explicit_image_query,
            relation=decision.relation,
            reason=decision.reason,
            text=request_text,
        )
        return should_reply_with_image, should_fallback_text_only

    def _looks_like_raw_image_followup(self, text: str, memory_summary: str) -> bool:
        normalized = (text or "").strip()
        if not normalized:
            return True
        raw_markers = (
            "图片展示",
            "手机屏幕显示",
            "界面显示",
            "当前题目为",
            "进度显示为",
            "底部导航栏",
            "顶部状态栏",
        )
        if any(marker in normalized for marker in raw_markers):
            return True
        if "；" in normalized and len(normalized) > 90:
            return True
        condensed_memory = re.sub(r"\s+", "", (memory_summary or "").strip())
        condensed_text = re.sub(r"\s+", "", normalized)
        if condensed_memory and condensed_text and condensed_text in condensed_memory:
            return True
        if condensed_memory and condensed_text and condensed_memory.startswith(condensed_text):
            return True
        return False

    def _track_task(
        self,
        coro,
        *,
        task_type: str = "background",
        source: str = "",
        metadata: dict[str, Any] | None = None,
        timeout_seconds: float | None = None,
        retries: int = 0,
    ) -> None:
        async def _managed(task_id: str) -> Any:
            await self.task_manager.update_progress(task_id, progress=0.05, message="started")
            try:
                result = await coro
                await self.task_manager.update_progress(task_id, progress=1.0, message="completed")
                self._trace("task.done", task_id=task_id, task_type=task_type, source=source)
                return result
            except Exception as exc:
                await self.task_manager.update_progress(task_id, progress=1.0, message=f"failed: {exc}")
                self._trace("task.error", task_id=task_id, task_type=task_type, source=source, error=str(exc))
                raise

        async def _register() -> None:
            task_id, task = await self.task_manager.create_task(
                task_type=task_type,
                coro_factory=_managed,
                source=source,
                metadata=metadata,
                timeout_seconds=timeout_seconds,
                retries=retries,
            )
            self._trace("task.start", task_id=task_id, task_type=task_type, source=source, metadata=metadata or {})
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)
            task.add_done_callback(self._log_background_task)

        task = asyncio.create_task(_register())
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        task.add_done_callback(self._log_background_task)

    def _log_background_task(self, task: asyncio.Task[Any]) -> None:
        try:
            task.result()
        except asyncio.CancelledError:
            pass
        except Exception:
            logger.exception("后台任务执行失败")

    def _ensure_background_workers(self) -> None:
        if self._memory_worker is not None and not self._memory_worker.done():
            self._trace("worker.state", worker="memory_maintenance", state="reused", pending=len(self._queued_memory_sessions))
            return
        self._memory_queue = asyncio.Queue()
        self._memory_worker = asyncio.create_task(self._memory_maintenance_loop())
        self._tasks.add(self._memory_worker)
        self._memory_worker.add_done_callback(self._tasks.discard)
        self._memory_worker.add_done_callback(self._log_background_task)
        self._trace("worker.state", worker="memory_maintenance", state="started", pending=0)

    def _enqueue_memory_maintenance(self, session_id: str, reason: str) -> None:
        if not session_id:
            return
        self._ensure_background_workers()
        if self._memory_queue is None or session_id in self._queued_memory_sessions:
            return
        self._queued_memory_sessions.add(session_id)
        self._memory_queue.put_nowait((session_id, reason))

    async def _memory_maintenance_loop(self) -> None:
        assert self._memory_queue is not None
        while True:
            item = await self._memory_queue.get()
            if item is None:
                return
            session_id, reason = item
            try:
                await asyncio.sleep(0.8)
                await self._run_memory_maintenance(session_id=session_id, reason=reason)
            finally:
                self._queued_memory_sessions.discard(session_id)

    async def _run_memory_maintenance(self, *, session_id: str, reason: str) -> None:
        await self._sync_context_embeddings(session_id=session_id, reason=reason)
        payload = self.conversation_store.get_maintenance_payload(
            session_id=session_id,
            hot_turns=self.config.memory_hot_turns,
            min_turns=self.config.memory_summary_min_turns,
            max_turns=self.config.memory_summary_batch_turns,
        )
        if payload is None:
            self._trace("memory.maintenance.skip", session_id=session_id, reason=reason)
            return
        self._trace(
            "memory.maintenance.start",
            session_id=session_id,
            reason=reason,
            start_index=payload["start_index"],
            end_index=payload["end_index"],
            model=self.tool_model.model_name,
        )
        result = await self.tool_model.summarize_memory_window(
            session_id=session_id,
            start_index=int(payload["start_index"]),
            end_index=int(payload["end_index"]),
            entries=list(payload["entries"]),
        )
        snapshot = self.conversation_store.store_summary(
            session_id=session_id,
            start_index=int(payload["start_index"]),
            end_index=int(payload["end_index"]),
            summary=str(result.get("summary") or ""),
            tags=list(result.get("tags") or []),
            source="tool_memory_worker",
        )
        if snapshot is None:
            self._trace("memory.maintenance.empty", session_id=session_id, reason=reason)
            return
        await self._sync_context_embeddings(session_id=session_id, reason=f"{reason}_summary")
        structured_stats = await self._refresh_structured_memory(
            session_id=session_id,
            entries=list(payload["entries"]),
            summary=snapshot.summary,
        )
        self._trace(
            "memory.maintenance.done",
            session_id=session_id,
            start_index=snapshot.start_index,
            end_index=snapshot.end_index,
            tags=snapshot.tags,
            summary=snapshot.summary,
            confidence=result.get("confidence"),
            structured_memories=structured_stats["memories"],
            profile_updates=structured_stats["profiles"],
        )

    async def _refresh_structured_memory(
        self,
        *,
        session_id: str,
        entries: list[dict[str, Any]],
        summary: str,
    ) -> dict[str, int]:
        try:
            self._trace(
                "memory.structured.start",
                session_id=session_id,
                entry_count=len(entries),
                model=self.tool_model.model_name,
            )
            result = await asyncio.wait_for(
                self.tool_model.extract_structured_memory(
                    session_id=session_id,
                    entries=entries,
                    summary=summary,
                ),
                timeout=25.0,
            )
            memories_saved = self.structured_memory_store.remember_many(
                session_id=session_id,
                memories=list(result.get("memories") or []),
                source="tool_structured_memory",
            )
            profiles_saved = self.structured_memory_store.upsert_profile_facts(
                scope_id=session_id,
                facts=list(result.get("profile") or []),
                source="tool_structured_memory",
            )
            self._trace(
                "memory.structured.done",
                session_id=session_id,
                memories_saved=memories_saved,
                profiles_saved=profiles_saved,
                summary=result.get("summary") or "",
            )
            return {"memories": memories_saved, "profiles": profiles_saved}
        except Exception as exc:
            logger.warning("结构化记忆刷新失败: %s", exc)
            self._trace(
                "memory.structured.error",
                session_id=session_id,
                error=str(exc),
            )
            return {"memories": 0, "profiles": 0}

    async def _sync_context_embeddings(self, *, session_id: str, reason: str) -> None:
        if self.context_embedding is None or not self.context_embedding.enabled:
            return
        pending = self.conversation_store.get_pending_embedding_documents(session_id=session_id, limit=12)
        if not pending:
            return
        self._trace(
            "context.embedding.sync.start",
            session_id=session_id,
            reason=reason,
            count=len(pending),
            model=self.context_embedding.model_name,
        )
        try:
            result = await self.context_embedding.embed_texts([str(item.get("text") or "") for item in pending])
            vectors = list(result.get("vectors") or [])
            if not vectors:
                self._trace("context.embedding.sync.empty", session_id=session_id, reason=reason)
                return
            records: list[dict[str, Any]] = []
            for item, vector in zip(pending, vectors):
                records.append(
                    {
                        "doc_id": item["doc_id"],
                        "session_id": session_id,
                        "kind": item["kind"],
                        "source_kind": item["source_kind"],
                        "timestamp": item["timestamp"],
                        "text": item["text"],
                        "tags": item.get("tags") or [],
                        "model": self.context_embedding.model_name,
                        "dimension": len(vector),
                        "vector": vector,
                    }
                )
            saved = self.conversation_store.save_embeddings(session_id=session_id, records=records)
            self._trace(
                "context.embedding.sync.done",
                session_id=session_id,
                reason=reason,
                count=saved,
                request_id=result.get("request_id"),
                usage=result.get("usage"),
            )
        except Exception as exc:
            logger.warning("上下文向量同步失败: %s", exc)
            self._trace(
                "context.embedding.sync.error",
                session_id=session_id,
                reason=reason,
                error=str(exc),
            )

    async def handle_napcat_payload(self, payload: dict[str, Any]) -> None:
        self._ensure_background_workers()
        if payload.get("meta_event_type") == "heartbeat":
            return
        if payload.get("post_type") != "message":
            return
        raw_message = str(payload.get("raw_message") or "").strip()
        if not raw_message:
            return
        request = TextRequest(
            source="NapCatQQ",
            text=raw_message,
            user_id=str(payload.get("user_id") or ""),
            message_type=str(payload.get("message_type") or "private"),
            group_id=str(payload.get("group_id")) if payload.get("group_id") is not None else None,
            extra={**payload, "normalized_text": self._strip_cq_codes(raw_message)},
        )
        self._trace(
            "chat.incoming",
            source=request.source,
            user_id=request.user_id,
            message_type=request.message_type,
            group_id=request.group_id,
            text=request.text,
        )
        normalized_message = raw_message.strip()
        lowered_message = normalized_message.lower()
        root_command = normalized_message[len("root/") :].strip() if normalized_message.startswith("root/") else ""
        direct_change_command = self._extract_change_command(normalized_message)
        root_change_command = self._extract_change_command(root_command) if root_command else None
        if root_change_command is not None:
            self._track_task(
                self._run_change_command(request, command_override=root_change_command),
                task_type="change.command",
                source=request.source,
                metadata={"user_id": request.user_id, "target_model": root_change_command},
            )
        elif normalized_message.startswith("root/"):
            self._track_task(self._run_root_command(request), task_type="root.command", source=request.source, metadata={"user_id": request.user_id})
        elif direct_change_command is not None:
            self._track_task(
                self._run_change_command(request, command_override=direct_change_command),
                task_type="change.command",
                source=request.source,
                metadata={"user_id": request.user_id, "target_model": direct_change_command},
            )
        elif lowered_message.startswith("approve "):
            approval_id = normalized_message.split(None, 1)[1].strip()
            self._track_task(
                self._run_approval_command(request, approval_id=approval_id, approve=True),
                task_type="approval.command",
                source=request.source,
                metadata={"approval_id": approval_id, "decision": "approve"},
            )
        elif lowered_message.startswith("reject "):
            approval_id = normalized_message.split(None, 1)[1].strip()
            self._track_task(
                self._run_approval_command(request, approval_id=approval_id, approve=False),
                task_type="approval.command",
                source=request.source,
                metadata={"approval_id": approval_id, "decision": "reject"},
            )
        elif lowered_message == "approvals":
            self._track_task(
                self._run_list_approvals(request),
                task_type="approval.list",
                source=request.source,
                metadata={"user_id": request.user_id},
            )
        elif "[CQ:image" in raw_message:
            self._track_task(self._run_napcat_image_message(request), task_type="napcat.image", source=request.source, metadata={"user_id": request.user_id})
        else:
            self._track_task(self._run_text_conversation(request), task_type="chat.turn", source=request.source, metadata={"user_id": request.user_id})

    async def handle_esp32_payload(self, payload: dict[str, Any]) -> None:
        self._ensure_background_workers()
        self._cleanup_stale_audio_streams()
        event_type = str(payload.get("type") or "unknown")
        device_id = str(payload.get("device_id") or "ESP32_Core_1")
        if event_type == "audio_text":
            content = str(payload.get("content") or "").strip()
            if not content:
                return
            await _send_esp32_status(
                self.connection_manager,
                status="thinking",
                device_id=device_id,
                text="收到文本，正在思考。",
            )
            request = TextRequest(source="ESP32", text=content, device_id=device_id, extra=payload)
            self._trace("chat.incoming", source=request.source, device_id=device_id, text=content)
            self._track_task(self._run_text_conversation(request), task_type="chat.turn", source=request.source, metadata={"device_id": device_id})
        elif event_type == "image":
            image_base64 = str(payload.get("image_base64") or payload.get("data") or "").strip()
            if not image_base64:
                logger.warning("ESP32 图片消息里没有图像数据（缺少 image_base64 或 data 字段）")
                return
            self._trace(
                "vision.frame.incoming",
                source="ESP32",
                device_id=device_id,
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                caption=str(payload.get("caption") or ""),
            )
            self._track_task(self._run_image_pipeline(device_id, payload), task_type="esp32.image", source="ESP32", metadata={"device_id": device_id})
        elif event_type == "telemetry":
            logger.info("收到 ESP32 状态上报 [%s]: %s", device_id, json.dumps(payload, ensure_ascii=False))
        elif event_type == "client_config":
            await self._handle_esp32_client_config(payload, device_id=device_id)
        elif event_type == "audio_stream_start":
            await self._handle_esp32_audio_stream_start(payload, device_id=device_id)
        elif event_type == "audio_stream_chunk":
            await self._handle_esp32_audio_stream_chunk(payload, device_id=device_id)
        elif event_type == "audio_stream_end":
            await self._handle_esp32_audio_stream_end(payload, device_id=device_id)
        else:
            logger.info("收到未处理的 ESP32 事件 %s，设备 %s", event_type, device_id)

    @staticmethod
    def _safe_config_id(raw: Any, default: str = "default") -> str:
        text = str(raw or "").strip().lower()
        text = re.sub(r"[^a-z0-9_-]+", "", text)
        return text[:48] or default

    @staticmethod
    def _bool_from_payload(value: Any) -> bool:
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return bool(value)
        return str(value or "").strip().lower() in {"1", "true", "yes", "on", "y"}

    async def _handle_esp32_client_config(self, payload: dict[str, Any], *, device_id: str) -> None:
        persona_id = self._safe_config_id(payload.get("persona_id"))
        voice_id = self._safe_config_id(payload.get("voice_id"))
        cfg = ESP32RuntimeConfig(
            persona_id=persona_id,
            persona_label=str(payload.get("persona_label") or persona_id)[:48],
            voice_id=voice_id,
            voice_label=str(payload.get("voice_label") or voice_id)[:48],
            continuous_chat=self._bool_from_payload(payload.get("continuous_chat")),
            wake_enabled=self._bool_from_payload(payload.get("wake_enabled")),
            wake_word=str(payload.get("wake_word") or "doubao")[:32],
        )
        self._esp32_runtime_config[device_id] = cfg
        self._trace(
            "esp32.config.updated",
            source="ESP32",
            device_id=device_id,
            persona_id=cfg.persona_id,
            voice_id=cfg.voice_id,
            continuous_chat=cfg.continuous_chat,
            wake_enabled=cfg.wake_enabled,
        )
        logger.info(
            "ESP32 配置更新 [%s]: persona=%s voice=%s continuous=%s wake=%s",
            device_id,
            cfg.persona_id,
            cfg.voice_id,
            cfg.continuous_chat,
            cfg.wake_enabled,
        )
        await self.connection_manager.send_to_esp32(
            {
                "type": "config_ack",
                "device_id": device_id,
                "persona_id": cfg.persona_id,
                "voice_id": cfg.voice_id,
                "continuous_chat": cfg.continuous_chat,
                "wake_enabled": cfg.wake_enabled,
            }
        )

    @staticmethod
    def _truncate_text(text: Any, limit: int) -> str:
        cleaned = re.sub(r"\s+", " ", str(text or "")).strip()
        if len(cleaned) <= limit:
            return cleaned
        return cleaned[: max(0, limit - 1)].rstrip() + "…"

    def _load_persona_prompt(self, persona_id: str) -> str:
        safe_id = self._safe_config_id(persona_id)
        if safe_id == "default":
            return ""
        persona_dir = self.config.doubao_dialog.persona_dir
        for suffix in (".md", ".txt"):
            path = persona_dir / f"{safe_id}{suffix}"
            try:
                if path.is_file():
                    return self._truncate_text(path.read_text(encoding="utf-8"), 1200)
            except Exception as exc:
                logger.warning("读取 ESP32 人设文件失败 %s: %s", path, exc)
                return ""
        return ""

    def _load_voice_presets(self) -> dict[str, str]:
        path = self.config.doubao_dialog.voice_preset_file
        try:
            mtime = path.stat().st_mtime
        except FileNotFoundError:
            self._voice_presets_cache = {}
            self._voice_presets_mtime = 0.0
            return {}
        except Exception as exc:
            logger.warning("读取音色预设文件状态失败 %s: %s", path, exc)
            return self._voice_presets_cache or {}
        if self._voice_presets_cache is not None and mtime == self._voice_presets_mtime:
            return self._voice_presets_cache
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            presets: dict[str, str] = {}
            if isinstance(raw, dict):
                for key, value in raw.items():
                    safe_key = self._safe_config_id(key)
                    if isinstance(value, dict):
                        speaker = str(value.get("speaker") or value.get("tts_speaker") or "").strip()
                    else:
                        speaker = str(value or "").strip()
                    if safe_key and speaker:
                        presets[safe_key] = speaker[:120]
            self._voice_presets_cache = presets
            self._voice_presets_mtime = mtime
            return presets
        except Exception as exc:
            logger.warning("读取音色预设文件失败 %s: %s", path, exc)
            self._voice_presets_cache = {}
            self._voice_presets_mtime = mtime
            return {}

    def _voice_speaker_for_config(self, cfg: ESP32RuntimeConfig) -> str:
        if cfg.voice_id == "default":
            return ""
        return self._load_voice_presets().get(cfg.voice_id, "")

    def _build_esp32_dialog_system_role(self, *, device_id: str, cfg: ESP32RuntimeConfig) -> str:
        session_id = self._session_id_for("ESP32", device_id=device_id)
        blocks = [self.config.doubao_dialog.system_role.strip()]
        persona_prompt = self._load_persona_prompt(cfg.persona_id)
        if persona_prompt:
            blocks.append(f"当前人设设定：\n{persona_prompt}")
        structured = self.structured_memory_store.build_context(
            session_id=session_id,
            profile_scope=session_id,
            profile_limit=3,
            memory_limit=3,
        )
        if structured:
            blocks.append("可参考的稳定记忆，按需使用，不要生硬复述：\n" + self._truncate_text(structured, 700))
        summaries = self.conversation_store.recent_summaries(session_id=session_id, limit=2)
        summary_lines = [
            self._truncate_text(item.get("summary") or item.get("text") or "", 120)
            for item in summaries
        ]
        summary_lines = [line for line in summary_lines if line]
        if summary_lines:
            blocks.append("近期对话摘要：\n" + "\n".join(f"- {line}" for line in summary_lines))
        blocks.append("实时语音回复要求：短句、自然、少铺垫。用户没要求详细解释时，优先一到三句话。")
        return "\n\n".join(block for block in blocks if block)

    def _build_esp32_dialog_history(self, *, device_id: str) -> list[dict[str, str]]:
        session_id = self._session_id_for("ESP32", device_id=device_id)
        recent = self.conversation_store.recent_entries(
            session_id=session_id,
            limit=min(6, max(2, self.config.context_recent_turns)),
        )
        compact: list[dict[str, str]] = []
        for turn in recent:
            user_text = self._truncate_text(turn.get("user_text"), 90)
            assistant_text = self._truncate_text(turn.get("assistant_text"), 90)
            if user_text or assistant_text:
                compact.append({"user_text": user_text, "assistant_text": assistant_text})
        return compact

    async def _handle_esp32_audio_stream_start(self, payload: dict[str, Any], *, device_id: str) -> None:
        session_id = str(payload.get("session_id") or "").strip()
        if not session_id:
            self._trace("audio.capture.error", source="ESP32", device_id=device_id, error="missing session_id")
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                text="录音上传缺少会话编号。",
            )
            return
        pcm_path = self.audio_capture_dir / f"{session_id}.pcm"
        pcm_path.write_bytes(b"")
        stream = PendingAudioStream(
            session_id=session_id,
            device_id=device_id,
            pcm_path=pcm_path,
            sample_rate=int(payload.get("sample_rate") or 16000),
            sample_bits=int(payload.get("sample_bits") or 16),
            channels=int(payload.get("channels") or 1),
            encoding=str(payload.get("encoding") or "pcm_s16le"),
        )
        self._pending_audio_streams[session_id] = stream
        self._trace(
            "audio.capture.start",
            source="ESP32",
            device_id=device_id,
            session_id=session_id,
            sample_rate=stream.sample_rate,
            sample_bits=stream.sample_bits,
            channels=stream.channels,
            encoding=stream.encoding,
        )
        await _send_esp32_status(
            self.connection_manager,
            status="uploading",
            device_id=device_id,
            session_id=session_id,
            text="正在上传录音。",
        )

    async def _handle_esp32_audio_stream_chunk(self, payload: dict[str, Any], *, device_id: str) -> None:
        session_id = str(payload.get("session_id") or "").strip()
        stream = self._pending_audio_streams.get(session_id)
        if stream is None:
            self._trace("audio.capture.error", source="ESP32", device_id=device_id, session_id=session_id, error="unknown audio session")
            return
        audio_b64 = str(payload.get("audio_b64") or "").strip()
        if not audio_b64:
            return
        try:
            chunk = base64.b64decode(audio_b64)
        except Exception as exc:
            self._trace("audio.capture.error", source="ESP32", device_id=device_id, session_id=session_id, error=f"chunk decode failed: {exc}")
            return
        with stream.pcm_path.open("ab") as handle:
            handle.write(chunk)
        stream.bytes_received += len(chunk)
        stream.updated_at = time.time()
        if stream.bytes_received % 32000 < len(chunk):
            self._trace(
                "audio.capture.chunk",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                bytes_received=stream.bytes_received,
            )

    async def _handle_esp32_audio_stream_end(self, payload: dict[str, Any], *, device_id: str) -> None:
        session_id = str(payload.get("session_id") or "").strip()
        stream = self._pending_audio_streams.pop(session_id, None)
        if stream is None:
            self._trace("audio.capture.error", source="ESP32", device_id=device_id, session_id=session_id, error="audio end without start")
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text="录音结束事件没有匹配的开始事件。",
            )
            return
        reason = str(payload.get("reason") or "").strip().lower()
        if reason in {"manual_stop", "cancel"}:
            try:
                stream.pcm_path.unlink(missing_ok=True)
            except TypeError:
                if stream.pcm_path.exists():
                    stream.pcm_path.unlink()
            self._trace(
                "audio.capture.cancelled",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                reason=reason,
                bytes_received=stream.bytes_received,
            )
            await _send_esp32_status(
                self.connection_manager,
                status="idle",
                device_id=device_id,
                session_id=session_id,
                text="已取消本次录音。",
            )
            return
        wav_path = self.audio_capture_dir / f"{session_id}.wav"
        try:
            pcm_bytes = stream.pcm_path.read_bytes()
            audio_stats = _analyze_pcm_s16le(pcm_bytes)
            with wave.open(str(wav_path), "wb") as wav_file:
                wav_file.setnchannels(max(1, stream.channels))
                wav_file.setsampwidth(max(1, stream.sample_bits // 8))
                wav_file.setframerate(max(8000, stream.sample_rate))
                wav_file.writeframes(pcm_bytes)
            try:
                stream.pcm_path.unlink(missing_ok=True)
            except TypeError:
                if stream.pcm_path.exists():
                    stream.pcm_path.unlink()
            self._trace(
                "audio.capture.saved",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                wav_path=str(wav_path),
                bytes_received=stream.bytes_received,
                duration_ms=int(payload.get("duration_ms") or 0),
                reason=str(payload.get("reason") or ""),
                audio_peak=audio_stats["peak"],
                audio_rms=round(float(audio_stats["rms"]), 2),
                audio_nonzero_ratio=round(float(audio_stats["nonzero_ratio"]), 6),
            )
            self._track_task(
                self._run_esp32_audio_pipeline(
                    session_id=session_id,
                    device_id=device_id,
                    wav_path=wav_path,
                    duration_ms=int(payload.get("duration_ms") or 0),
                ),
                task_type="esp32.audio_asr",
                source="ESP32",
                metadata={"device_id": device_id, "session_id": session_id},
            )
        except Exception as exc:
            logger.exception("ESP32 录音收尾失败")
            self._trace("audio.capture.error", source="ESP32", device_id=device_id, session_id=session_id, error=str(exc))
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text=f"录音收尾失败: {exc}",
            )

    async def _run_esp32_audio_pipeline(
        self,
        *,
        session_id: str,
        device_id: str,
        wav_path: Path,
        duration_ms: int,
    ) -> None:
        if self.config.doubao_dialog.enabled:
            if not self.doubao_dialog.available:
                self._trace(
                    "dialog.config.error",
                    source="ESP32",
                    device_id=device_id,
                    session_id=session_id,
                    error="Doubao realtime dialog credentials are not configured",
                )
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "assistant_error",
                        "device_id": device_id,
                        "session_id": session_id,
                        "error": "豆包端到端实时语音没有配置完整，请检查 DOUBAO_DIALOG_APP_ID / DOUBAO_DIALOG_APP_KEY / DOUBAO_DIALOG_ACCESS_TOKEN。",
                    }
                )
                await _send_esp32_status(
                    self.connection_manager,
                    status="error",
                    device_id=device_id,
                    session_id=session_id,
                    text="豆包实时语音配置不完整。",
                )
                return
            handled = await self._run_esp32_doubao_dialog_pipeline(
                session_id=session_id,
                device_id=device_id,
                wav_path=wav_path,
                duration_ms=duration_ms,
            )
            if handled:
                return
            return

        if self.asr_model is None:
            self._trace(
                "audio.asr.error",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                error="ASR model not configured",
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text="ASR 模型没有配置。",
            )
            return
        try:
            wav_bytes = wav_path.read_bytes()
            audio_stats = _analyze_wav_audio(wav_bytes)
        except Exception as exc:
            self._trace(
                "audio.asr.error",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                error=f"read wav failed: {exc}",
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text=f"读取录音失败: {exc}",
            )
            return
        if int(audio_stats.get("nonzero_samples") or 0) == 0 or int(audio_stats.get("peak") or 0) == 0:
            silent_message = (
                "captured audio appears silent: "
                f"peak={int(audio_stats.get('peak') or 0)}, "
                f"rms={round(float(audio_stats.get('rms') or 0.0), 2)}, "
                f"nonzero_ratio={round(float(audio_stats.get('nonzero_ratio') or 0.0), 6)}"
            )
            self._trace(
                "audio.asr.error",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                error=silent_message,
            )
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_error",
                    "device_id": device_id,
                    "error": "这段录音几乎是静音，服务端收到的 PCM 全是空白。请检查麦克风/I2S 接线、采样通道和增益。",
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text="这段录音几乎是静音。",
            )
            return

        self._trace(
            "audio.asr.start",
            source="ESP32",
            device_id=device_id,
            session_id=session_id,
            wav_path=str(wav_path),
            duration_ms=duration_ms,
            model=self.asr_model.model_name,
        )
        await _send_esp32_status(
            self.connection_manager,
            status="asr",
            device_id=device_id,
            session_id=session_id,
            text="正在识别语音。",
        )
        try:
            result = await self.asr_model.transcribe_wav(
                wav_bytes,
                mime_type="audio/wav",
                prompt="请转写这段来自 ESP32 麦克风的中文语音，直接输出识别文本。",
            )
            transcript = str(result.get("text") or "").strip()
            self._trace(
                "audio.asr.done",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                model=self.asr_model.model_name,
                transcript=transcript,
            )
        except Exception as exc:
            logger.exception("ESP32 语音转写失败")
            self._trace(
                "audio.asr.error",
                source="ESP32",
                device_id=device_id,
                session_id=session_id,
                model=self.asr_model.model_name,
                error=str(exc),
            )
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_error",
                    "device_id": device_id,
                    "error": f"语音识别失败: {exc}",
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text=f"语音识别失败: {exc}",
            )
            return

        if not transcript:
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_error",
                    "device_id": device_id,
                    "error": "刚刚这段语音我没听清，你再说一次试试。",
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text="刚刚这段语音没有识别出文本。",
            )
            return
        await _send_esp32_status(
            self.connection_manager,
            status="thinking",
            device_id=device_id,
            session_id=session_id,
            text=transcript,
        )

        request = TextRequest(
            source="ESP32",
            text=transcript,
            device_id=device_id,
            extra={
                "normalized_text": transcript,
                "audio_session_id": session_id,
                "audio_wav_path": str(wav_path),
                "audio_duration_ms": duration_ms,
                "audio_transcript": transcript,
                "input_modality": "speech",
            },
        )
        self._trace(
            "chat.incoming",
            source="ESP32",
            device_id=device_id,
            text=transcript,
            modality="speech",
        )
        await self._run_text_conversation(request)

    async def _run_esp32_doubao_dialog_pipeline(
        self,
        *,
        session_id: str,
        device_id: str,
        wav_path: Path,
        duration_ms: int,
    ) -> bool:
        try:
            with wave.open(str(wav_path), "rb") as wav_file:
                source_channels = wav_file.getnchannels()
                source_rate = wav_file.getframerate()
                source_bits = wav_file.getsampwidth() * 8
                pcm = wav_file.readframes(wav_file.getnframes())
            if source_bits != 16 or source_channels not in (1, 2) or source_rate <= 0:
                raise RuntimeError(f"unsupported ESP32 capture format: rate={source_rate} ch={source_channels} bits={source_bits}")
            pcm16 = _resample_pcm_s16le_mono(
                pcm,
                source_rate=source_rate,
                source_channels=source_channels,
                target_rate=16000,
            )
            audio_stats = _analyze_pcm_s16le(pcm16)
            if int(audio_stats.get("nonzero_samples") or 0) == 0 or int(audio_stats.get("peak") or 0) == 0:
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "assistant_error",
                        "device_id": device_id,
                        "error": "这段录音几乎是静音，服务端收到的 PCM 全是空白。",
                    }
                )
                await _send_esp32_status(
                    self.connection_manager,
                    status="error",
                    device_id=device_id,
                    session_id=session_id,
                    text="这段录音几乎是静音。",
                )
                return True
        except Exception as exc:
            logger.exception("ESP32 豆包实时语音准备失败")
            self._trace("dialog.prepare.error", source="ESP32", device_id=device_id, session_id=session_id, error=str(exc))
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text=f"实时语音准备失败: {exc}",
            )
            return True

        runtime_cfg = self._esp32_runtime_config.get(device_id) or ESP32RuntimeConfig()
        dialog_history = self._build_esp32_dialog_history(device_id=device_id)
        system_role = self._build_esp32_dialog_system_role(device_id=device_id, cfg=runtime_cfg)
        tts_speaker = self._voice_speaker_for_config(runtime_cfg)
        if runtime_cfg.voice_id != "default" and not tts_speaker:
            logger.warning(
                "ESP32 请求音色 %s，但未在 %s 找到 speaker，使用实例默认音色。",
                runtime_cfg.voice_id,
                self.config.doubao_dialog.voice_preset_file,
            )

        self._trace(
            "dialog.incoming",
            source="ESP32",
            device_id=device_id,
            session_id=session_id,
            wav_path=str(wav_path),
            duration_ms=duration_ms,
            audio_peak=audio_stats["peak"],
            audio_rms=round(float(audio_stats["rms"]), 2),
            persona_id=runtime_cfg.persona_id,
            voice_id=runtime_cfg.voice_id,
            voice_speaker=tts_speaker,
            history_turns=len(dialog_history),
        )

        chunk_no = 0
        audio_pcm_buffer = bytearray()
        debug_pcm_chunks: list[bytes] = []
        sent_pcm_start = False
        final_asr = ""
        assistant_text_parts: list[str] = []
        sent_dialog_statuses: set[str] = set()

        async def send_dialog_status_once(status: str, text: str = "") -> None:
            if status in sent_dialog_statuses:
                return
            sent_dialog_statuses.add(status)
            await _send_esp32_status(
                self.connection_manager,
                status=status,
                device_id=device_id,
                session_id=session_id,
                text=text,
            )

        async def flush_dialog_audio(*, final: bool = False) -> None:
            nonlocal chunk_no, sent_pcm_start
            source_rate = self.config.doubao_dialog.tts_sample_rate
            source_channels = self.config.doubao_dialog.tts_channel
            min_bytes = source_rate * 2 * source_channels * self.config.doubao_dialog.output_flush_ms // 1000
            if not audio_pcm_buffer:
                return
            if not final and len(audio_pcm_buffer) < min_bytes:
                return
            pcm_chunk = bytes(audio_pcm_buffer)
            audio_pcm_buffer.clear()
            if self.config.doubao_dialog.tts_format != "pcm_s16le":
                raise RuntimeError(f"unsupported Doubao dialog TTS format: {self.config.doubao_dialog.tts_format}")
            if pcm_chunk.startswith(b"OggS"):
                raise RuntimeError(
                    "Doubao dialog returned OGG/Opus audio; StartSession tts.audio_config pcm_s16le did not take effect"
                )
            if pcm_chunk[:4] == b"RIFF" or pcm_chunk[:3] == b"ID3":
                raise RuntimeError(f"Doubao dialog returned unexpected encoded audio header: {pcm_chunk[:12]!r}")
            esp32_pcm = _resample_pcm_s16le_mono(
                pcm_chunk,
                source_rate=source_rate,
                source_channels=source_channels,
                target_rate=_ESP32_TTS_SAMPLE_RATE,
                target_peak=_DIALOG_TTS_TARGET_PEAK,
                max_gain=_DIALOG_TTS_MAX_GAIN,
            )
            if not esp32_pcm:
                return
            debug_pcm_chunks.append(esp32_pcm)
            dialog_stats = _analyze_pcm_s16le(esp32_pcm)
            logger.info(
                "Doubao dialog PCM ready for ESP32: chunk=%d source_bytes=%d source_rate=%d source_channels=%d "
                "esp32_bytes=%d esp32_rate=%d peak=%d rms=%.2f gain_limit=%.1f",
                chunk_no,
                len(pcm_chunk),
                source_rate,
                source_channels,
                len(esp32_pcm),
                _ESP32_TTS_SAMPLE_RATE,
                int(dialog_stats.get("peak") or 0),
                float(dialog_stats.get("rms") or 0.0),
                _DIALOG_TTS_MAX_GAIN,
            )
            if not sent_pcm_start:
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "tts_pcm_start",
                        "device_id": device_id,
                        "session_id": session_id,
                        "sample_rate": _ESP32_TTS_SAMPLE_RATE,
                        "sample_bits": 16,
                        "channels": 1,
                        "format": "pcm_s16le",
                        "voice": "doubao-dialog",
                        "text": final_asr[:120],
                    }
                )
                sent_pcm_start = True
            await _emit_esp32_tts_pcm_chunk(
                self.connection_manager.send_to_esp32,
                device_id=device_id,
                session_id=session_id,
                chunk_no=chunk_no,
                pcm=esp32_pcm,
            )
            chunk_no += 1

        try:
            await send_dialog_status_once("asr", "豆包实时语音识别中。")
            async for event in self.doubao_dialog.run_pcm_dialog(
                pcm16,
                input_sample_rate=16000,
                device_id=device_id,
                session_id=session_id,
                history=dialog_history,
                system_role_override=system_role,
                tts_speaker_override=tts_speaker,
                bot_name_override=self.config.doubao_dialog.bot_name,
            ):
                payload = event.payload
                if event.event == EVENT_ASR_INFO:
                    await send_dialog_status_once("asr", "开始识别。")
                elif event.event == EVENT_ASR_RESPONSE and isinstance(payload, dict):
                    results = payload.get("results") or []
                    if results:
                        text = str(results[0].get("text") or "").strip()
                        if text:
                            final_asr = text
                            if not bool(results[0].get("is_interim")):
                                await send_dialog_status_once("thinking", text[:120])
                elif event.event == EVENT_ASR_ENDED:
                    await send_dialog_status_once("thinking", final_asr or "正在思考。")
                elif event.event == EVENT_TTS_SENTENCE_START:
                    await send_dialog_status_once("tts", "豆包实时合成中。")
                elif event.event == EVENT_CHAT_RESPONSE and isinstance(payload, dict):
                    content = str(payload.get("content") or "")
                    if content:
                        assistant_text_parts.append(content)
                elif event.event == EVENT_TTS_RESPONSE:
                    if isinstance(event.payload, (bytes, bytearray)):
                        audio_pcm_buffer.extend(bytes(event.payload))
                        await flush_dialog_audio()
                elif event.event == EVENT_TTS_SENTENCE_END:
                    await flush_dialog_audio(final=True)
                elif event.event == EVENT_CHAT_ENDED:
                    final_reply = "".join(assistant_text_parts).strip()
                    if final_reply:
                        await self.connection_manager.send_to_esp32(
                            {
                                "type": "assistant_done",
                                "device_id": device_id,
                                "session_id": session_id,
                                "text": final_reply,
                            }
                        )
                elif event.event == EVENT_TTS_ENDED:
                    await flush_dialog_audio(final=True)
                    if debug_pcm_chunks:
                        _save_tts_debug_wav(
                            _pcm_to_wav(b"".join(debug_pcm_chunks), sample_rate=_ESP32_TTS_SAMPLE_RATE)
                        )
                    if sent_pcm_start:
                        await self.connection_manager.send_to_esp32(
                            {
                                "type": "tts_pcm_end",
                                "device_id": device_id,
                                "session_id": session_id,
                                "chunks": chunk_no,
                            }
                        )
                elif event.event == EVENT_SESSION_FAILED:
                    raise RuntimeError(f"Doubao dialog session failed: {payload!r}")
        except Exception as exc:
            logger.exception("ESP32 豆包实时语音失败")
            self._trace("dialog.error", source="ESP32", device_id=device_id, session_id=session_id, error=str(exc))
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_error",
                    "device_id": device_id,
                    "session_id": session_id,
                    "error": f"豆包实时语音失败: {exc}",
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="error",
                device_id=device_id,
                session_id=session_id,
                text=f"豆包实时语音失败: {exc}",
            )
            return True

        final_reply = "".join(assistant_text_parts).strip()
        if final_asr or final_reply:
            self.conversation_store.append_turn(
                session_id=self._session_id_for("ESP32", device_id=device_id),
                source="ESP32",
                user_text=final_asr or "[voice]",
                assistant_text=final_reply or "",
            )
        await _send_esp32_status(
            self.connection_manager,
            status="idle",
            device_id=device_id,
            session_id=session_id,
            text=final_reply or final_asr,
        )
        self._trace(
            "dialog.done",
            source="ESP32",
            device_id=device_id,
            session_id=session_id,
            transcript=final_asr,
            assistant_text=final_reply,
        )
        return True

    async def _run_change_command(self, request: TextRequest, command_override: str | None = None) -> None:
        try:
            command = (command_override or self._extract_change_command(request.text) or "").strip()
            if not command:
                await self._send_qq_reply(
                    request,
                    "切模型命令后面需要输入模型编号或名称。可用写法: change/mimo、change_model:mimo、exchange/mimo、change/gpt-5.4",
                )
                return
            
            raw_result = await self.agent_bridge.run_change_model_command(command)
            await self._send_qq_reply(request, raw_result.strip())
        except Exception as exc:
            logger.exception("切换模型命令执行失败")
            await self._send_qq_reply(request, f"切换模型失败: {exc}")

    async def _run_list_approvals(self, request: TextRequest) -> None:
        approvals = await self.approval_manager.list_pending(source=request.source, user_id=request.user_id)
        if not approvals:
            await self._send_qq_reply(request, "当前没有待确认的高危操作。")
            return
        lines = ["当前待确认操作："]
        for item in approvals[:10]:
            lines.append(f"- {item['approval_id']}: {item['action_type']} | {item['summary']}")
        lines.append("回复 approve <id> 执行，或 reject <id> 拒绝。")
        await self._send_qq_reply(request, "\n".join(lines))

    async def _run_approval_command(self, request: TextRequest, *, approval_id: str, approve: bool) -> None:
        approval_id = approval_id.strip()
        if not approval_id:
            await self._send_qq_reply(request, "请带上审批编号，例如 approve ab12cd34")
            return
        if approve:
            approval, executor = await self.approval_manager.approve(approval_id, source=request.source, user_id=request.user_id)
            if approval is None or executor is None:
                await self._send_qq_reply(request, f"没有找到待批准的操作: {approval_id}")
                return
            self._trace(
                "approval.approved",
                approval_id=approval.approval_id,
                action_type=approval.action_type,
                source=request.source,
                user_id=request.user_id,
            )
            await self._send_qq_reply(request, f"已批准 {approval.approval_id}，开始执行：{approval.summary}")
            self._track_task(
                executor(),
                task_type=f"approved.{approval.action_type}",
                source=request.source,
                metadata={"approval_id": approval.approval_id, **approval.metadata},
            )
            return
        approval = await self.approval_manager.reject(approval_id, source=request.source, user_id=request.user_id)
        if approval is None:
            await self._send_qq_reply(request, f"没有找到待拒绝的操作: {approval_id}")
            return
        self._trace(
            "approval.rejected",
            approval_id=approval.approval_id,
            action_type=approval.action_type,
            source=request.source,
            user_id=request.user_id,
        )
        await self._send_qq_reply(request, f"已拒绝 {approval.approval_id}：{approval.summary}")

    async def _request_high_risk_approval(
        self,
        *,
        request: TextRequest,
        action_type: str,
        summary: str,
        metadata: dict[str, Any],
        executor,
    ) -> bool:
        if not self.config.high_risk_approval_enabled or request.source != "NapCatQQ":
            return False
        approval = await self.approval_manager.create(
            action_type=action_type,
            summary=summary,
            source=request.source,
            user_id=request.user_id,
            session_id=self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id),
            metadata=metadata,
            executor=executor,
        )
        self._trace(
            "approval.pending",
            approval_id=approval.approval_id,
            action_type=action_type,
            source=request.source,
            user_id=request.user_id,
            summary=summary,
        )
        await self._send_qq_reply(
            request,
            f"这是高危操作，先确认一下：\n{summary}\n审批编号: {approval.approval_id}\n回复 approve {approval.approval_id} 执行，或 reject {approval.approval_id} 取消。",
        )
        return True

    async def _run_root_command(self, request: TextRequest) -> None:
        try:
            command = request.text[len("root/") :].strip()
            if not command:
                await self._send_qq_reply(request, "root/ 后面没有命令。")
                return
            if await self._request_high_risk_approval(
                request=request,
                action_type="root_command",
                summary=f"root agent 执行项目级命令: {command}",
                metadata={"command": command},
                executor=lambda: self._execute_root_command(request, command),
            ):
                return
            await self._execute_root_command(request, command)
        except Exception as exc:
            logger.exception("高级指令执行失败")
            self._trace("root.command.error", command=request.text, error=str(exc))
            await self._send_qq_reply(request, f"root agent 执行失败: {exc}")

    async def _execute_root_command(self, request: TextRequest, command: str) -> None:
        try:
            self._trace(
                "root.command.start",
                command=command,
                source=request.source,
                user_id=request.user_id,
                agent_python=str(self.config.generic_agent_python),
            )
            await self._send_qq_reply(request, f"已唤醒 root agent，开始处理：{command}\n你可以继续聊天，任务完成后我再把结果回给你。")
            bridge_prompt = (
                f"你正在作为 root agent 处理来自 NapCat 的项目级命令。\n"
                f"主项目根目录: {self.config.project_root}\n"
                f"GenericAgent 目录: {self.config.generic_agent_root}\n"
                "优先处理主项目根目录里的文件，跨目录操作时请明确说明。\n"
                "只在任务真正完成后再给出结果，不要把中间计划、工具调用或脚本草稿当最终答复。\n"
                f"用户命令: {command}"
            )
            raw_result = await self.agent_bridge.run_root_command(bridge_prompt)
            message = raw_result
            if self._should_summarize_root_result(raw_result):
                message = await self.language_model.summarize_agent_result(
                    command=command,
                    raw_result=raw_result,
                    max_chars=900,
                )
            if not message.strip():
                message = raw_result.strip() or "root agent 已执行，但没有返回可展示内容。"
            self._trace("root.command.done", command=command, raw_result=raw_result, result=message)
            await self._send_qq_reply(request, message)
        except Exception as exc:
            logger.exception("高级指令执行失败")
            self._trace("root.command.error", command=command, error=str(exc))
            await self._send_qq_reply(request, f"root agent 执行失败: {exc}")

    async def _run_image_pipeline(self, device_id: str, payload: dict[str, Any]) -> None:
        try:
            caption = str(payload.get("caption") or "").strip()
            frame = self.frame_store.save_frame(
                device_id=device_id,
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                image_base64=str(payload.get("image_base64") or payload.get("data") or ""),
                note=caption,
            )
            analysis = await self.vision_model.observe_environment(
                image_base64=str(payload.get("image_base64") or payload.get("data") or ""),
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                source_hint="esp32-camera",
                caption=caption,
            )
            self.frame_store.update_summary(frame.frame_id, analysis.scene_summary)
            self.conversation_store.append_note(
                session_id=self._session_id_for("ESP32", device_id=device_id),
                source="ESP32",
                summary=f"收到图像并完成视觉观察: {analysis.scene_summary}",
                tags=analysis.tags,
            )
            self._enqueue_memory_maintenance(self._session_id_for("ESP32", device_id=device_id), "esp32_vision_note")
            self._trace(
                "vision.observe",
                frame_id=frame.frame_id,
                device_id=device_id,
                model=self.vision_model.model_name,
                summary=analysis.scene_summary,
                should_reply=False,
                confidence=analysis.confidence,
                importance=analysis.importance,
            )
            if analysis.device_action:
                await self.connection_manager.send_to_esp32({"type": "device_command", "command": analysis.device_action})
            self.memory_store.remember(
                source=f"vision:{device_id}",
                summary=analysis.scene_summary,
                confidence=analysis.confidence,
                importance=analysis.importance,
                tags=analysis.tags,
            )
            await self.connection_manager.send_to_esp32(
                {
                    "type": "vision_memory_update",
                    "device_id": device_id,
                    "frame_id": frame.frame_id,
                    "summary": analysis.scene_summary,
                    "confidence": analysis.confidence,
                }
            )
            self._trace(
                "vision.observe.memory_only",
                device_id=device_id,
                frame_id=frame.frame_id,
                summary=analysis.scene_summary,
            )
            if caption:
                request = TextRequest(source="ESP32", text=caption, device_id=device_id, extra=payload)
                context_bundle = await self._assemble_request_context(request, caption)
                should_reply_with_image, should_fallback_text_only = await self._score_image_reply_relevance(
                    request=request,
                    request_text=caption,
                    image_summary=analysis.scene_summary,
                    context_bundle=context_bundle,
                )
                if should_reply_with_image:
                    await self._run_text_conversation(
                        request,
                        vision_analysis=analysis,
                        context_bundle=context_bundle,
                    )
                    return
                if should_fallback_text_only:
                    await self._run_text_conversation(
                        request,
                        context_bundle=context_bundle,
                    )
        except Exception as exc:
            logger.exception("ESP32 图像处理流程失败")
            self._trace("vision.observe.error", device_id=device_id, error=str(exc))
            await self.connection_manager.send_to_esp32(
                {
                    "type": "vision_error",
                    "device_id": device_id,
                    "error": str(exc),
                }
            )

    async def _run_napcat_image_message(self, request: TextRequest) -> None:
        try:
            started_at = time.perf_counter()
            image_urls = self._extract_cq_image_urls(request.text)
            plain_text = self._strip_cq_codes(request.text)
            session_id = self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id)
            if not image_urls:
                await self._run_text_conversation(request)
                return
            frame = await self._download_napcat_image(image_urls[0], request)
            if frame is None:
                if plain_text:
                    request.text = plain_text
                    await self._run_text_conversation(request)
                else:
                    self._trace(
                        "vision.observe.qq.download_miss",
                        source=request.source,
                        user_id=request.user_id,
                        group_id=request.group_id,
                        image_url=image_urls[0],
                    )
                return
            pending_job = self._register_pending_image_job(request, session_id=session_id, frame_id=frame.frame_id)
            self._trace(
                "vision.observe.qq.pending",
                source=request.source,
                user_id=request.user_id,
                group_id=request.group_id,
                frame_id=frame.frame_id,
                chat_key=pending_job.chat_key,
            )
            analysis = await self.vision_highres_model.observe_environment(
                image_base64=frame.image_base64,
                mime_type=frame.mime_type,
                source_hint="napcat-qq-image-highres",
                caption=plain_text,
            )
            self.frame_store.update_summary(frame.frame_id, analysis.scene_summary)
            self.frame_store.update_highres_summary(frame.frame_id, analysis.scene_summary)
            self.frame_store.update_knowledge_summary(frame.frame_id, analysis.scene_summary)
            self.memory_store.remember(
                source=f"napcat_image:{request.user_id or request.group_id or 'unknown'}",
                summary=analysis.scene_summary,
                confidence=analysis.confidence,
                importance=max(analysis.importance, 0.55),
                tags=analysis.tags,
            )
            self.conversation_store.append_note(
                session_id=session_id,
                source=request.source,
                summary=f"QQ 图片视觉摘要: {analysis.scene_summary}",
                tags=["qq_image", *analysis.tags],
            )
            self._enqueue_memory_maintenance(session_id, "napcat_image_note")
            self._trace(
                "vision.observe.qq",
                source=request.source,
                user_id=request.user_id,
                group_id=request.group_id,
                frame_id=frame.frame_id,
                model=self.vision_highres_model.model_name,
                summary=analysis.scene_summary,
                confidence=analysis.confidence,
                elapsed_ms=int((time.perf_counter() - started_at) * 1000),
            )
            pending_job.status = "completed"
            pending_job.updated_at = time.time()
            if plain_text:
                request.text = plain_text
                context_bundle = await self._assemble_request_context(request, plain_text)
                should_reply_with_image, should_fallback_text_only = await self._score_image_reply_relevance(
                    request=request,
                    request_text=plain_text,
                    image_summary=analysis.scene_summary,
                    context_bundle=context_bundle,
                )
                if should_reply_with_image:
                    await self._run_text_conversation(
                        request,
                        vision_analysis=analysis,
                        context_bundle=context_bundle,
                    )
                    return
                if should_fallback_text_only:
                    await self._run_text_conversation(
                        request,
                        context_bundle=context_bundle,
                    )
                return
            if pending_job.waiting_for_followup and not pending_job.completion_sent:
                proactive_text = await self._compose_natural_image_followup(
                    user_text=pending_job.latest_query or "刚才那张图片是什么？",
                    memory_summary=analysis.scene_summary,
                )
                pending_job.completion_sent = True
                pending_job.updated_at = time.time()
                self._trace(
                    "vision.observe.qq.followup",
                    source=request.source,
                    user_id=request.user_id,
                    group_id=request.group_id,
                    frame_id=frame.frame_id,
                    query=pending_job.latest_query,
                    text=proactive_text,
                )
                await self._send_qq_reply(request, proactive_text)
                return
            preface_text = self._find_recent_image_preface(request)
            if preface_text:
                proactive_text = await self._compose_natural_image_followup(
                    user_text=preface_text,
                    memory_summary=analysis.scene_summary,
                )
                pending_job.latest_query = preface_text
                pending_job.completion_sent = True
                pending_job.updated_at = time.time()
                self._trace(
                    "vision.observe.qq.preface_followup",
                    source=request.source,
                    user_id=request.user_id,
                    group_id=request.group_id,
                    frame_id=frame.frame_id,
                    preface=preface_text,
                    text=proactive_text,
                )
                await self._send_qq_reply(request, proactive_text)
                return
            context_query = self._find_recent_image_context_query(request)
            if context_query:
                contextual_request = TextRequest(
                    source=request.source,
                    text=context_query,
                    user_id=request.user_id,
                    device_id=request.device_id,
                    message_type=request.message_type,
                    group_id=request.group_id,
                    extra={"normalized_text": context_query},
                )
                context_bundle = await self._assemble_request_context(contextual_request, context_query)
                should_reply_with_image, _ = await self._score_image_reply_relevance(
                    request=contextual_request,
                    request_text=context_query,
                    image_summary=analysis.scene_summary,
                    context_bundle=context_bundle,
                )
                if should_reply_with_image:
                    proactive_text = await self._compose_natural_image_followup(
                        user_text=context_query,
                        memory_summary=analysis.scene_summary,
                    )
                    pending_job.latest_query = context_query
                    pending_job.completion_sent = True
                    pending_job.updated_at = time.time()
                    self._trace(
                        "vision.observe.qq.context_followup",
                        source=request.source,
                        user_id=request.user_id,
                        group_id=request.group_id,
                        frame_id=frame.frame_id,
                        query=context_query,
                        text=proactive_text,
                    )
                    await self._send_qq_reply(request, proactive_text)
                    return
            self._trace(
                "vision.observe.qq.memory_only",
                source=request.source,
                user_id=request.user_id,
                group_id=request.group_id,
                frame_id=frame.frame_id,
                summary=analysis.scene_summary,
            )
            return
        except Exception as exc:
            logger.exception("QQ 图片处理流程失败")
            self._trace("vision.observe.qq.error", source=request.source, text=request.text, error=str(exc))
            plain_text = self._strip_cq_codes(request.text)
            if plain_text:
                request.text = plain_text
                await self._run_text_conversation(request)

    async def _generate_and_dispatch_reply(
        self,
        *,
        request: TextRequest,
        request_text: str,
        session_id: str,
        semantic_plan: SemanticPlan,
        subconscious_context: str,
        tool_bundle: ToolExecutionBundle,
        vision_analysis: VisionAnalysis | None = None,
        offline_gap_candidate: OfflineGapAnalysisCandidate | None = None,
    ) -> None:
        esp32_ready = bool(getattr(self.connection_manager, "is_esp32_connected", True))
        audio_session_id = str(request.extra.get("audio_session_id") or "")
        tts_dispatcher = (
            StreamingTTSDispatcher(
                self.tts_model,
                self.connection_manager.send_to_esp32,
                device_id=request.device_id if request.source == "ESP32" else "",
                session_id=audio_session_id if request.source == "ESP32" else "",
                segment_stream_end=request.source == "ESP32",
            )
            if self.tts_model and esp32_ready
            else None
        )
        reply_chunks: list[str] = []
        tool_context = tool_bundle.to_prompt_block()
        vision_context = ""
        if vision_analysis is not None:
            vision_context = (
                "以下内容是内部视觉线索，只能作为回答依据，请重新组织成自然口语，不要逐字复述。\n"
                f"场景摘要: {vision_analysis.scene_summary}\n"
                f"建议方向: {vision_analysis.suggested_reply}"
            )

        if request.source == "ESP32":
            await _send_esp32_status(
                self.connection_manager,
                status="thinking",
                device_id=request.device_id,
                session_id=audio_session_id,
                text=request_text,
            )

        last_tts_flush_len = 0
        async for chunk in self.language_model.stream_reply(
            source=request.source,
            user_text=request_text,
            subconscious=subconscious_context,
            tool_context=tool_context,
            vision_context=vision_context,
            response_style=semantic_plan.response_style,
        ):
            reply_chunks.append(chunk)
            self._trace(
                "assistant.delta",
                source=request.source,
                device_id=request.device_id,
                model=self.language_model.model_name,
                text=chunk,
            )
            if request.source == "ESP32":
                self._trace("chat.outgoing.esp32.delta", device_id=request.device_id, text=chunk)
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "assistant_text_delta",
                        "device_id": request.device_id,
                        "text": chunk,
                    }
                )
            if tts_dispatcher is not None:
                await tts_dispatcher.push_text(chunk)
                if len("".join(reply_chunks)) - last_tts_flush_len >= 12 and chunk.strip().endswith(("。", "！", "？", "!", "?")):
                    await tts_dispatcher.force_flush()
                    last_tts_flush_len = len("".join(reply_chunks))

        final_reply = "".join(reply_chunks).strip()
        if not final_reply and request.source == "NapCatQQ":
            pending_job, hold_reply = self._mark_pending_image_detail_request(request, request_text)
            if pending_job is not None and hold_reply:
                self._trace(
                    "chat.reply.empty_fallback",
                    source=request.source,
                    user_id=request.user_id,
                    group_id=request.group_id,
                    frame_id=pending_job.frame_id,
                    query=request_text,
                    fallback=hold_reply,
                )
                final_reply = hold_reply
        if not final_reply:
            final_reply = "我刚刚这句没组织好，你再说一句，我马上接上。"
        if request.source == "ESP32":
            final_reply = _sanitize_spoken_text(final_reply) or "我刚刚这句没组织好，你再说一句。"
        self._trace(
            "chat.record",
            source=request.source,
            model=self.language_model.model_name,
            user_text=request.text,
            assistant_text=final_reply,
            tool_context=tool_context,
        )
        if tts_dispatcher is not None:
            await tts_dispatcher.finish()
        if (
            request.source == "ESP32"
            and self.tts_model is not None
            and esp32_ready
            and (tts_dispatcher is None or tts_dispatcher.sent_audio_segments == 0)
        ):
            await _send_single_tts_segment(self.tts_model, self.connection_manager.send_to_esp32, final_reply)
        self.conversation_store.append_turn(
            session_id=session_id,
            source=request.source,
            user_text=request_text,
            assistant_text=final_reply,
        )
        self._enqueue_memory_maintenance(session_id, "dialogue_turn")
        if offline_gap_candidate is not None:
            self._track_task(
                self._run_offline_gap_analysis(candidate=offline_gap_candidate, return_assistant_text=final_reply),
                task_type="temporal.offline_gap_analysis",
                source=request.source,
                metadata={"session_id": session_id, "gap_minutes": round(offline_gap_candidate.gap_minutes, 2)},
            )

        if request.source == "NapCatQQ":
            qq_summary = ""
            if self._should_build_qq_summary(final_reply):
                qq_summary = await self.language_model.summarize_for_qq(
                    user_text=request_text,
                    assistant_reply=final_reply,
                    max_chars=self.config.qq_summary_max_chars,
                )
            message = self._format_qq_message(final_reply, qq_summary)
            await self._send_qq_reply(request, message)
        else:
            self._trace("chat.outgoing.esp32.done", device_id=request.device_id, text=final_reply)
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_done",
                    "device_id": request.device_id,
                    "text": final_reply,
                    "tool_context": tool_context,
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="idle",
                device_id=request.device_id,
                session_id=audio_session_id,
                text=final_reply,
            )

    async def _execute_tool_plan(
        self,
        *,
        request: TextRequest,
        request_text: str,
        session_id: str,
        semantic_plan: SemanticPlan,
        subconscious_context: str,
        tool_plan: ToolPlan,
        vision_analysis: VisionAnalysis | None = None,
        offline_gap_candidate: OfflineGapAnalysisCandidate | None = None,
    ) -> None:
        tool_bundle = await self.tool_registry.execute_plan(tool_plan.calls)
        self._trace(
            "tool.result",
            source=request.source,
            results=[{"tool": item.tool_name, "ok": item.ok, "content": item.content, "error": item.error} for item in tool_bundle.results],
        )
        for device_payload in [*tool_bundle.device_payloads, *tool_plan.device_commands]:
            await self.connection_manager.send_to_esp32({"type": "device_command", "command": device_payload})
        if tool_plan.direct_reply and not tool_plan.return_to_language:
            await self._dispatch_direct_reply(request, tool_plan.direct_reply)
            return
        await self._generate_and_dispatch_reply(
            request=request,
            request_text=request_text,
            session_id=session_id,
            semantic_plan=semantic_plan,
            subconscious_context=subconscious_context,
            tool_bundle=tool_bundle,
            vision_analysis=vision_analysis,
            offline_gap_candidate=offline_gap_candidate,
        )

    async def _run_text_conversation(
        self,
        request: TextRequest,
        *,
        vision_analysis: VisionAnalysis | None = None,
        context_bundle: RequestContextBundle | None = None,
    ) -> None:
        try:
            request_text = str(request.extra.get("normalized_text") or request.text).strip() or request.text
            pending_image_job = self._get_pending_image_job(request) if request.source == "NapCatQQ" else None
            recent_frame_hint = self._get_recent_frame_for_request(request) if request.source == "NapCatQQ" else None
            referential_hint = recent_frame_hint is not None and self._looks_like_referential_followup(request_text)
            if (
                request.source == "NapCatQQ"
                and vision_analysis is None
                and self._looks_like_image_preface(request_text)
                and pending_image_job is None
                and not referential_hint
                and not self._looks_like_image_detail_query(request_text)
            ):
                ack_text = self._build_image_preface_ack(request_text)
                ack_session_id = self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id, device_id=request.device_id)
                offline_gap_candidate = self._prepare_offline_gap_analysis(
                    session_id=ack_session_id,
                    source=request.source,
                    return_user_text=request_text,
                )
                self.conversation_store.append_turn(
                    session_id=ack_session_id,
                    source=request.source,
                    user_text=request_text,
                    assistant_text=ack_text,
                )
                self._enqueue_memory_maintenance(
                    ack_session_id,
                    "dialogue_turn",
                )
                if offline_gap_candidate is not None:
                    self._track_task(
                        self._run_offline_gap_analysis(candidate=offline_gap_candidate, return_assistant_text=ack_text),
                        task_type="temporal.offline_gap_analysis",
                        source=request.source,
                        metadata={"session_id": ack_session_id, "gap_minutes": round(offline_gap_candidate.gap_minutes, 2)},
                    )
                self._trace(
                    "chat.preface_ack",
                    source=request.source,
                    user_id=request.user_id,
                    group_id=request.group_id,
                    user_text=request_text,
                    assistant_text=ack_text,
                )
                await self._send_qq_reply(request, ack_text)
                return
            context_bundle = context_bundle or await self._assemble_request_context(request, request_text)
            session_id = context_bundle.session_id
            offline_gap_candidate = self._prepare_offline_gap_analysis(
                session_id=session_id,
                source=request.source,
                return_user_text=request_text,
            )
            recent_frame = context_bundle.recent_frame
            referential_followup = context_bundle.referential_followup
            if request.source == "NapCatQQ" and vision_analysis is None and (
                self._looks_like_image_detail_query(request_text) or referential_followup
            ):
                pending_job, hold_reply = self._mark_pending_image_detail_request(request, request_text)
                if pending_job is not None and hold_reply:
                    self._trace(
                        "vision.observe.qq.pending_query",
                        source=request.source,
                        user_id=request.user_id,
                        group_id=request.group_id,
                        frame_id=pending_job.frame_id,
                        query=request_text,
                        hold_reply=hold_reply,
                    )
                    await self._send_qq_reply(request, hold_reply)
                    return
            subconscious_context = context_bundle.subconscious_context
            self._trace(
                "context.assembled",
                session_id=session_id,
                source=request.source,
                request_text=request_text,
                has_conversation=bool(context_bundle.conversation_context),
                has_frame=bool(context_bundle.latest_frame_context),
                has_subconscious=bool(subconscious_context),
                has_temporal=bool(context_bundle.temporal_context),
                temporal_gap_minutes=round(context_bundle.temporal_gap_minutes, 2),
            )
            if vision_analysis is None:
                semantic_plan: SemanticPlan | None = None
                fast_route = ""
                semantic_plan = self._detect_fast_tool_route(request_text)
                if semantic_plan is not None:
                    fast_route = f"tool:{semantic_plan.intent}"
                elif self._is_fast_chat_candidate(request_text) or (
                    request.source == "ESP32"
                    and self.config.esp32_realtime_mode
                    and "\n" not in request_text
                    and len(request_text.strip()) <= self.config.esp32_realtime_fast_chat_max_chars
                ):
                    semantic_plan = SemanticPlan(
                        intent="chat",
                        needs_tools=False,
                        should_reply=True,
                        tool_goal=request_text,
                        response_style="像即时聊天一样简短自然，直接回答。",
                    )
                    fast_route = "chat"

                if semantic_plan is not None:
                    self._trace(
                        "semantic.plan.fast_route",
                        source=request.source,
                        user_text=request_text,
                        model="fast_path",
                        route=fast_route,
                        intent=semantic_plan.intent,
                        needs_tools=semantic_plan.needs_tools,
                    )
                else:
                    semantic_plan = await self.language_model.plan_text(
                        source=request.source,
                        user_text=request_text,
                        subconscious=subconscious_context,
                    )
            else:
                semantic_plan = await self.language_model.plan_text(
                    source=request.source,
                    user_text=request_text,
                    subconscious=subconscious_context,
                )
            self._trace(
                "semantic.plan",
                source=request.source,
                user_text=request_text,
                model=self.language_model.model_name,
                intent=semantic_plan.intent,
                needs_tools=semantic_plan.needs_tools,
                should_reply=semantic_plan.should_reply,
                tool_goal=semantic_plan.tool_goal,
            )
            if not semantic_plan.should_reply:
                self.memory_store.remember(
                    source=request.source.lower(),
                    summary=f"未回复消息保留上下文: {request_text}",
                    confidence=0.35,
                    importance=0.2,
                )
                return

            if semantic_plan.needs_tools:
                tool_plan = await self.tool_model.plan_tools(
                    user_text=request_text,
                    semantic_plan=semantic_plan,
                    tool_catalog=self.tool_registry.catalog(),
                    extra_context=context_bundle.latest_frame_context,
                )
                self._trace(
                    "tool.plan",
                    source=request.source,
                    user_text=request_text,
                    model=self.tool_model.model_name,
                    calls=tool_plan.calls,
                    return_to_language=tool_plan.return_to_language,
                    direct_reply=tool_plan.direct_reply,
                    device_commands=tool_plan.device_commands,
                )
                high_risk_calls = self.tool_registry.approval_requirements(tool_plan.calls)
                if tool_plan.device_commands:
                    high_risk_calls.append(
                        {
                            "name": "device_command",
                            "arguments": tool_plan.device_commands,
                            "risk_level": "high",
                            "requires_approval": True,
                            "can_direct_device": True,
                            "tags": ["device_control"],
                        }
                    )
                if high_risk_calls and await self._request_high_risk_approval(
                    request=request,
                    action_type="tool_plan",
                    summary=f"执行高危工具计划: {', '.join(item['name'] for item in high_risk_calls)}",
                    metadata={"request_text": request_text, "tools": high_risk_calls},
                    executor=lambda: self._execute_tool_plan(
                        request=request,
                        request_text=request_text,
                        session_id=session_id,
                        semantic_plan=semantic_plan,
                        subconscious_context=subconscious_context,
                        tool_plan=tool_plan,
                        vision_analysis=vision_analysis,
                        offline_gap_candidate=offline_gap_candidate,
                    ),
                ):
                    return
                await self._execute_tool_plan(
                    request=request,
                    request_text=request_text,
                    session_id=session_id,
                    semantic_plan=semantic_plan,
                    subconscious_context=subconscious_context,
                    tool_plan=tool_plan,
                    vision_analysis=vision_analysis,
                    offline_gap_candidate=offline_gap_candidate,
                )
                return

            await self._generate_and_dispatch_reply(
                request=request,
                request_text=request_text,
                session_id=session_id,
                semantic_plan=semantic_plan,
                subconscious_context=subconscious_context,
                tool_bundle=ToolExecutionBundle(),
                vision_analysis=vision_analysis,
                offline_gap_candidate=offline_gap_candidate,
            )
        except Exception as exc:
            logger.exception("对话处理流程失败")
            self._trace("chat.error", source=request.source, text=request_text, error=str(exc))
            if request.source == "NapCatQQ":
                await self._send_qq_reply(request, f"本轮处理失败: {exc}")
            else:
                await _send_esp32_status(
                    self.connection_manager,
                    status="error",
                    device_id=request.device_id,
                    session_id=str(request.extra.get("audio_session_id") or ""),
                    text=f"本轮处理失败: {exc}",
                )
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "assistant_error",
                        "device_id": request.device_id,
                        "error": str(exc),
                    }
                )

    async def _dispatch_direct_reply(self, request: TextRequest, text: str) -> None:
        if request.source == "ESP32":
            text = _sanitize_spoken_text(text) or "好的。"
        self._trace("chat.direct_reply", source=request.source, text=text)
        if request.source == "NapCatQQ":
            await self._send_qq_reply(request, text)
        else:
            esp32_ready = bool(getattr(self.connection_manager, "is_esp32_connected", True))
            await _send_esp32_status(
                self.connection_manager,
                status="thinking",
                device_id=request.device_id,
                text=text,
            )
            if self.tts_model and esp32_ready:
                await _send_single_tts_segment(self.tts_model, self.connection_manager.send_to_esp32, text)
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_done",
                    "device_id": request.device_id,
                    "text": text,
                }
            )
            await _send_esp32_status(
                self.connection_manager,
                status="idle",
                device_id=request.device_id,
                text=text,
            )

    async def _send_qq_reply(self, request: TextRequest, text: str) -> None:
        text = self._sanitize_for_qq(text)
        if not text.strip():
            self._trace(
                "chat.outgoing.qq.empty_guard",
                source=request.source,
                user_id=request.user_id,
                group_id=request.group_id,
            )
            text = "我这边刚整理完，但这句输出空了，你再戳我一下，我马上接上。"
        action = "send_group_msg" if request.message_type == "group" and request.group_id else "send_private_msg"
        params: dict[str, Any] = {"message": text}
        if action == "send_group_msg":
            group_id = request.group_id or ""
            params["group_id"] = int(group_id) if group_id.isdigit() else group_id
        else:
            user_id = request.user_id
            params["user_id"] = int(user_id) if user_id.isdigit() else user_id
        self._trace("chat.outgoing.qq", action=action, params=params)
        await self.connection_manager.send_to_qq({"action": action, "params": params})

    def _format_qq_message(self, final_reply: str, qq_summary: str) -> str:
        if not qq_summary:
            return final_reply
        if qq_summary.strip() == final_reply.strip():
            return final_reply
        return f"{final_reply}\n\n[本轮摘要]\n{qq_summary}"

    def _sanitize_for_qq(self, text: str) -> str:
        cleaned = (text or "").replace("\r\n", "\n").replace("\r", "\n")
        replacements = {
            "├──": "-",
            "└──": "-",
            "│": "|",
            "—": "-",
            "•": "-",
            "·": "-",
            "🚀": "",
            "⚙️": "",
            "🧠": "",
            "🔄": "",
            "🖼️": "",
            "💾": "",
            "🌉": "",
            "▶️": "",
            "🔑": "",
            "📦": "",
            "🔧": "",
            "📂": "",
            "📁": "",
        }
        for src, dst in replacements.items():
            cleaned = cleaned.replace(src, dst)
        cleaned = re.sub(r"\[(.*?)\]\((.*?)\)", r"\1", cleaned)
        cleaned = re.sub(r"^#{1,6}\s*", "", cleaned, flags=re.MULTILINE)
        cleaned = re.sub(r"^\s*[-*_]{3,}\s*$", "", cleaned, flags=re.MULTILINE)
        cleaned = re.sub(r"^\s*[*+-]\s+", "- ", cleaned, flags=re.MULTILINE)
        cleaned = self._flatten_table_lines(cleaned)
        cleaned = cleaned.replace("**", "").replace("__", "").replace("`", "")
        cleaned = "".join(ch for ch in cleaned if not self._should_drop_qq_char(ch))
        cleaned = re.sub(r"[\x00-\x08\x0B-\x1F\x7F]", "", cleaned)
        cleaned = re.sub(r"\n{3,}", "\n\n", cleaned).strip()
        return cleaned

    def _session_id_for(
        self,
        source: str,
        *,
        user_id: str = "",
        group_id: str | None = None,
        device_id: str = "",
    ) -> str:
        if source in {"NapCatQQ", "ESP32"}:
            return self.config.shared_session_id
        return f"{source.lower()}:{device_id or 'default'}"

    def _extract_cq_image_urls(self, text: str) -> list[str]:
        urls: list[str] = []
        for match in re.finditer(r"\[CQ:image,([^\]]+)\]", text):
            params = match.group(1)
            url_match = re.search(r"url=([^,\]]+)", params)
            if url_match:
                urls.append(html.unescape(url_match.group(1)))
        return urls

    def _strip_cq_codes(self, text: str) -> str:
        stripped = re.sub(r"\[CQ:[^\]]+\]", " ", text)
        return re.sub(r"\s+", " ", stripped).strip()

    async def _download_napcat_image(self, url: str, request: TextRequest):
        try:
            async with httpx.AsyncClient(timeout=30.0, follow_redirects=True) as client:
                response = await client.get(url)
                response.raise_for_status()
            content_type = response.headers.get("content-type", "image/jpeg").split(";")[0].strip() or "image/jpeg"
            image_base64 = base64.b64encode(response.content).decode("utf-8")
            device_id = self._session_id_for(request.source, user_id=request.user_id, group_id=request.group_id)
            frame = self.frame_store.save_frame(
                device_id=device_id,
                mime_type=content_type,
                image_base64=image_base64,
                note="NapCat 图片消息",
            )
            return frame
        except Exception as exc:
            logger.warning("下载 QQ 图片失败: %s", exc)
            return None

    def _should_drop_qq_char(self, ch: str) -> bool:
        if ch in "\n\t ":
            return False
        if "\u4e00" <= ch <= "\u9fff":
            return False
        category = unicodedata.category(ch)
        if category in {"So", "Sk", "Cf", "Cs"}:
            return True
        return False

    def _flatten_table_lines(self, text: str) -> str:
        lines: list[str] = []
        for line in text.split("\n"):
            stripped = line.strip()
            if stripped.startswith("|") and stripped.endswith("|"):
                cells = [cell.strip() for cell in stripped.strip("|").split("|")]
                if cells and all(cell and set(cell) <= {"-", ":", " "} for cell in cells):
                    continue
                compact = " / ".join(cell for cell in cells if cell)
                if compact:
                    lines.append(compact)
                    continue
            lines.append(line)
        return "\n".join(lines)
