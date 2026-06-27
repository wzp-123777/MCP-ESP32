from __future__ import annotations

import asyncio
import gzip
import inspect
import io
import json
import logging
import struct
import uuid
from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any

import websockets

from app_config import DoubaoDialogConfig


logger = logging.getLogger("MCP_Robot.DoubaoDialog")

_CLIENT_FULL_REQUEST = 0b0001
_CLIENT_AUDIO_ONLY_REQUEST = 0b0010
_SERVER_FULL_RESPONSE = 0b1001
_SERVER_ACK = 0b1011
_SERVER_ERROR_RESPONSE = 0b1111

_MSG_WITH_EVENT = 0b0100
_JSON = 0b0001
_NO_SERIALIZATION = 0b0000
_GZIP = 0b0001

_CONNECTION_EVENTS = {1, 2, 50, 51, 52}

EVENT_START_CONNECTION = 1
EVENT_FINISH_CONNECTION = 2
EVENT_START_SESSION = 100
EVENT_FINISH_SESSION = 102
EVENT_TASK_REQUEST = 200

EVENT_CONNECTION_STARTED = 50
EVENT_CONNECTION_FAILED = 51
EVENT_CONNECTION_FINISHED = 52
EVENT_SESSION_STARTED = 150
EVENT_SESSION_FINISHED = 152
EVENT_SESSION_FAILED = 153
EVENT_SESSION_USAGE = 154
EVENT_TTS_SENTENCE_START = 350
EVENT_TTS_SENTENCE_END = 351
EVENT_TTS_RESPONSE = 352
EVENT_TTS_ENDED = 359
EVENT_ASR_INFO = 450
EVENT_ASR_RESPONSE = 451
EVENT_ASR_ENDED = 459
EVENT_CHAT_RESPONSE = 550
EVENT_CHAT_ENDED = 559


@dataclass(slots=True)
class DialogEvent:
    event: int
    message_type: int
    session_id: str = ""
    payload: Any = None
    error_code: int = 0


def _pack_event_frame(
    event: int,
    payload: bytes | str = b"{}",
    *,
    session_id: str = "",
    message_type: int = _CLIENT_FULL_REQUEST,
    serialization: int = _JSON,
    compression: int = _GZIP,
) -> bytes:
    frame = bytearray(
        [
            (1 << 4) | 1,
            (message_type << 4) | _MSG_WITH_EVENT,
            (serialization << 4) | compression,
            0,
        ]
    )
    frame.extend(int(event).to_bytes(4, "big", signed=True))
    if event not in _CONNECTION_EVENTS:
        sid = session_id.encode("utf-8")
        frame.extend(len(sid).to_bytes(4, "big", signed=True))
        frame.extend(sid)
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if compression == _GZIP:
        payload = gzip.compress(payload)
    frame.extend(len(payload).to_bytes(4, "big", signed=False))
    frame.extend(payload)
    return bytes(frame)


def _parse_event_frame(data: bytes | str) -> DialogEvent:
    if isinstance(data, str):
        raise ValueError(f"unexpected text websocket frame: {data[:120]!r}")
    if len(data) < 8:
        raise ValueError(f"dialog frame too short: {len(data)}")
    header_size = (data[0] & 0x0F) * 4
    message_type = data[1] >> 4
    flags = data[1] & 0x0F
    serialization = data[2] >> 4
    compression = data[2] & 0x0F
    buf = io.BytesIO(data[header_size:])
    event = 0
    session_id = ""
    error_code = 0

    if message_type in (_SERVER_FULL_RESPONSE, _SERVER_ACK):
        if flags & _MSG_WITH_EVENT:
            event = int.from_bytes(buf.read(4), "big", signed=True)
        raw = buf.read(4)
        if raw:
            sid_len = int.from_bytes(raw, "big", signed=True)
            if sid_len > 0:
                session_id = buf.read(sid_len).decode("utf-8", errors="replace")
    elif message_type == _SERVER_ERROR_RESPONSE:
        error_code = int.from_bytes(buf.read(4), "big", signed=False)
    else:
        raise ValueError(f"unexpected dialog message type: {message_type}")

    size_raw = buf.read(4)
    payload_bytes = b""
    if size_raw:
        payload_size = int.from_bytes(size_raw, "big", signed=False)
        if payload_size:
            payload_bytes = buf.read(payload_size)

    if compression == _GZIP and payload_bytes:
        payload_bytes = gzip.decompress(payload_bytes)
    payload: Any = payload_bytes
    if serialization == _JSON and payload_bytes:
        payload = json.loads(payload_bytes.decode("utf-8"))
    return DialogEvent(
        event=event,
        message_type=message_type,
        session_id=session_id,
        payload=payload,
        error_code=error_code,
    )


def _pcm_to_wav(pcm: bytes, *, sample_rate: int = 16000, channels: int = 1, bits_per_sample: int = 16) -> bytes:
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


class DoubaoRealtimeDialogClient:
    def __init__(self, config: DoubaoDialogConfig) -> None:
        self.config = config

    @property
    def available(self) -> bool:
        return bool(
            self.config.enabled
            and self.config.app_id
            and self.config.app_key
            and self.config.access_token
        )

    async def run_pcm_dialog(
        self,
        pcm: bytes,
        *,
        input_sample_rate: int = 16000,
        device_id: str = "",
        session_id: str = "",
        history: list[dict[str, str]] | None = None,
        system_role_override: str = "",
        tts_speaker_override: str = "",
        bot_name_override: str = "",
    ) -> AsyncIterator[DialogEvent]:
        if not self.available:
            raise RuntimeError("Doubao realtime dialog credentials are not configured")
        if input_sample_rate != 16000:
            raise RuntimeError(f"Doubao dialog currently expects 16k PCM, got {input_sample_rate}")

        request_id = str(uuid.uuid4())
        connect_id = str(uuid.uuid4())
        dialog_session_id = session_id or str(uuid.uuid4())
        headers = {
            "X-Api-App-Id": self.config.app_id,
            "X-Api-App-Key": self.config.app_key,
            "X-Api-Access-Key": self.config.access_token,
            "X-Api-Resource-Id": self.config.resource_id,
            "X-Api-Request-Id": request_id,
            "X-Api-Connect-Id": connect_id,
        }
        connect_kwargs: dict[str, Any] = {
            "open_timeout": self.config.timeout_seconds,
            "close_timeout": 5,
            "max_size": 16 * 1024 * 1024,
            "ping_interval": None,
        }
        header_arg = "additional_headers" if "additional_headers" in str(inspect.signature(websockets.connect)) else "extra_headers"
        connect_kwargs[header_arg] = headers
        logger.info("Doubao realtime dialog connect: device=%s session=%s", device_id, dialog_session_id)

        async with websockets.connect(self.config.ws_url, **connect_kwargs) as ws:
            await ws.send(_pack_event_frame(EVENT_START_CONNECTION))
            event = _parse_event_frame(await asyncio.wait_for(ws.recv(), self.config.timeout_seconds))
            if event.event != EVENT_CONNECTION_STARTED:
                raise RuntimeError(f"Doubao dialog connection failed: event={event.event} payload={event.payload!r}")

            system_role = system_role_override.strip() or self.config.system_role
            if history:
                history_lines = ["以下是之前的对话记录，请参考上下文回答："]
                for turn in history:
                    user = turn.get("user_text", "").strip()
                    assistant = turn.get("assistant_text", "").strip()
                    if user:
                        history_lines.append(f"用户：{user}")
                    if assistant:
                        history_lines.append(f"助手：{assistant}")
                system_role = system_role + "\n\n" + "\n".join(history_lines)

            start_payload = {
                "dialog": {
                    "bot_name": bot_name_override.strip() or self.config.bot_name,
                    "system_role": system_role,
                    "extra": {
                        "input_mod": self.config.input_mod or "keep_alive",
                    },
                },
                "tts": {
                    "speaker": tts_speaker_override.strip() or self.config.tts_speaker,
                    "audio_config": {
                        "channel": self.config.tts_channel,
                        "format": self.config.tts_format,
                        "sample_rate": self.config.tts_sample_rate,
                    }
                },
                "asr": {
                    "audio_info": {
                        "format": "pcm",
                        "sample_rate": input_sample_rate,
                        "channel": 1,
                    }
                },
            }
            await ws.send(
                _pack_event_frame(
                    EVENT_START_SESSION,
                    json.dumps(start_payload, ensure_ascii=False),
                    session_id=dialog_session_id,
                )
            )
            event = _parse_event_frame(await asyncio.wait_for(ws.recv(), self.config.timeout_seconds))
            if event.event != EVENT_SESSION_STARTED:
                raise RuntimeError(f"Doubao dialog session failed: event={event.event} payload={event.payload!r}")

            audio_task = asyncio.create_task(self._send_audio(ws, pcm, dialog_session_id), name="doubao-dialog-send-audio")
            try:
                while True:
                    event = _parse_event_frame(await asyncio.wait_for(ws.recv(), self.config.timeout_seconds))
                    yield event
                    if event.message_type == _SERVER_ERROR_RESPONSE:
                        raise RuntimeError(f"Doubao dialog server error {event.error_code}: {event.payload!r}")
                    if event.event in (EVENT_TTS_ENDED, EVENT_SESSION_FAILED, EVENT_SESSION_FINISHED):
                        break
            finally:
                if not audio_task.done():
                    audio_task.cancel()
                try:
                    await audio_task
                except asyncio.CancelledError:
                    pass
                try:
                    await ws.send(_pack_event_frame(EVENT_FINISH_SESSION, session_id=dialog_session_id))
                except Exception:
                    pass

    async def _send_audio(self, ws: Any, pcm: bytes, session_id: str) -> None:
        bytes_per_ms = 16000 * 2 // 1000
        chunk_bytes = max(640, bytes_per_ms * self.config.audio_chunk_ms)
        chunk_bytes -= chunk_bytes % 2
        tail = b"\x00\x00" * int(16000 * self.config.vad_tail_silence_ms / 1000)
        payload = pcm + tail
        logger.info(
            "Doubao dialog audio upload: session=%s pcm=%d tail=%d chunk=%d gzip=1",
            session_id,
            len(pcm),
            len(tail),
            chunk_bytes,
        )
        for offset in range(0, len(payload), chunk_bytes):
            chunk = payload[offset : offset + chunk_bytes]
            if not chunk:
                continue
            await ws.send(
                _pack_event_frame(
                    EVENT_TASK_REQUEST,
                    chunk,
                    session_id=session_id,
                    message_type=_CLIENT_AUDIO_ONLY_REQUEST,
                    serialization=_NO_SERIALIZATION,
                    compression=_GZIP,
                )
            )
            await asyncio.sleep(max(0.01, self.config.audio_chunk_ms / 1000 * 0.4))


def dialog_audio_event_to_wav(event: DialogEvent, *, sample_rate: int = 16000) -> bytes:
    if event.event != EVENT_TTS_RESPONSE or not isinstance(event.payload, (bytes, bytearray)):
        return b""
    return _pcm_to_wav(bytes(event.payload), sample_rate=sample_rate)
