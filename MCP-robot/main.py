from __future__ import annotations

import asyncio
import re
import json
import logging
import os
import socket
import sys
import threading
import time
from collections import deque
from html import escape
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any
import wave

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from starlette.websockets import WebSocketState

from app_config import load_config
from mcp_facade import build_mcp_facade
from runtime import RobotRuntime, _analyze_wav_audio


def _enable_utf8_console_io() -> None:
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        reconfigure = getattr(stream, "reconfigure", None)
        if callable(reconfigure):
            try:
                reconfigure(encoding="utf-8", errors="replace")
            except Exception:
                pass


_enable_utf8_console_io()
config = load_config()
config.data_dir.mkdir(parents=True, exist_ok=True)


def _short_text(value: Any, limit: int = 80) -> str:
    text = " ".join(str(value or "").split())
    if len(text) <= limit:
        return text
    return text[: limit - 3] + "..."


def _translate_worker_name(name: Any) -> str:
    mapping = {
        "memory_maintenance": "记忆整理任务",
    }
    text = str(name or "").strip()
    return mapping.get(text, text or "后台任务")


def _translate_worker_state(state: Any) -> str:
    mapping = {
        "started": "已启动",
        "ready": "已就绪",
        "reused": "继续复用",
        "done": "已完成",
        "failed": "失败",
        "reset": "已重置",
    }
    text = str(state or "").strip().lower()
    return mapping.get(text, text or "未知状态")


def _translate_task_type(task_type: Any) -> str:
    mapping = {
        "approval.command": "处理审批回复",
        "approval.list": "查看待审批事项",
        "change.command": "切换模型",
        "chat.turn": "处理一轮对话",
        "esp32.audio_asr": "ESP32 语音转写",
        "esp32.image": "ESP32 图像理解",
        "napcat.image": "QQ 图片理解",
        "root.command": "执行高级指令",
        "temporal.offline_gap_analysis": "离线时段分析",
    }
    text = str(task_type or "").strip()
    return mapping.get(text, text or "后台任务")


def _translate_approval_action(action_type: Any) -> str:
    mapping = {
        "root_command": "高级指令",
        "tool_plan": "工具执行方案",
    }
    text = str(action_type or "").strip()
    return mapping.get(text, text or "待审批操作")


def _translate_intent(intent: Any) -> str:
    mapping = {
        "chat": "普通对话",
        "tool": "工具调用",
        "vision": "图像理解",
        "image_followup": "图片追问",
        "root_command": "高级指令",
    }
    text = str(intent or "").strip()
    return mapping.get(text, text or "未分类")


def _humanize_error_text(raw: Any) -> str:
    text = _short_text(raw, limit=160)
    lowered = text.lower()
    if "captured audio appears silent" in lowered:
        return "服务端收到的是静音音频，请检查麦克风/I2S 接线、采样通道和增益"
    if "does not support this input" in lowered:
        return "当前服务暂时不支持这种输入格式"
    if "internalerror.algo.invalidparameter" in lowered or "invalid_parameter" in lowered:
        return "请求参数不符合当前服务要求"
    if "missing session_id" in lowered:
        return "缺少会话编号 session_id"
    if "unknown audio session" in lowered:
        return "没有找到对应的录音会话"
    if "chunk decode failed" in lowered:
        return "音频分片解码失败"
    if "audio end without start" in lowered:
        return "先收到了录音结束信号，但没有对应的开始信号"
    if "audio stream expired before completion" in lowered:
        return "录音上传中途超时，系统已放弃本次录音"
    if "timeout" in lowered:
        return "请求超时"
    return text or "未知原因"


def _translate_event_label(event: str) -> str:
    mapping = {
        "audio.capture.error": "ESP32 录音处理",
        "audio.asr.error": "ESP32 语音识别",
        "chat.error": "对话处理",
        "context.embedding.query.error": "长期记忆检索",
        "context.embedding.sync.error": "长期记忆写入",
        "root.command.error": "高级指令执行",
        "task.error": "后台任务",
        "temporal.offline_gap.error": "离线时段分析",
        "vision.observe.error": "ESP32 图像理解",
        "vision.observe.qq.error": "QQ 图片理解",
    }
    return mapping.get(event, event)


def _build_trace_details(event: str, payload: dict[str, Any]) -> str:
    detail_specs: list[tuple[str, str, Any]] = [
        ("source", "来源", None),
        ("device_id", "设备", None),
        ("session_id", "会话", None),
        ("action", "动作", None),
        ("target", "目标", None),
        ("echo", "回声", None),
        ("status", "状态", None),
        ("retcode", "返回码", None),
        ("wording", "提示", lambda value: _short_text(value, 80)),
        ("task_type", "任务", _translate_task_type),
        ("worker", "后台任务", _translate_worker_name),
        ("state", "状态", _translate_worker_state),
        ("model", "模型", None),
        ("query", "查询", lambda value: _short_text(value, 40)),
        ("text", "文本", lambda value: _short_text(value, 60)),
        ("transcript", "转写", lambda value: _short_text(value, 60)),
        ("assistant_text", "回复", lambda value: _short_text(value, 80)),
        ("entry_count", "窗口条数", None),
        ("gap_minutes", "离线分钟", lambda value: f"{float(value):.1f}"),
        ("confidence", "置信度", lambda value: f"{float(value):.2f}"),
        ("memories_saved", "会话记忆新增", None),
        ("profiles_saved", "画像新增", None),
        ("pre_gap_state", "离线前状态", lambda value: _short_text(value, 70)),
        ("return_fit", "回归拟合", lambda value: _short_text(value, 70)),
        ("prompt_preview", "注入预览", lambda value: _short_text(value, 80)),
        ("summary", "分析摘要", lambda value: _short_text(value, 80)),
        ("audio_peak", "峰值", None),
        ("audio_rms", "均方根", None),
        ("audio_nonzero_ratio", "非零占比", lambda value: f"{float(value) * 100:.2f}%"),
        ("error", "原因", _humanize_error_text),
    ]
    parts: list[str] = []
    for key, label, formatter in detail_specs:
        value = payload.get(key)
        if value in (None, "", [], {}):
            continue
        display = formatter(value) if callable(formatter) else value
        if display in (None, "", [], {}):
            continue
        if key == "text" and event == "chat.incoming":
            continue
        if key == "assistant_text" and event == "chat.record":
            continue
        parts.append(f"{label}={display}")
    return "；".join(parts[:5])


def _render_trace_event(event: str, payload: dict[str, Any]) -> str:
    source = payload.get("source") or ""
    model = payload.get("model") or ""
    if event == "chat.incoming":
        text = _short_text(payload.get("text") or "", 80)
        return f"{source or '通道'} 收到消息: {text}"
    if event == "context.embedding.query":
        dims = payload.get("dims") or ""
        return f"{model or '记忆检索模型'} 正在检索长期记忆，向量维度 {dims}"
    if event == "context.embedding.sync.start":
        count = payload.get("count") or 0
        return f"{model or '记忆写入模型'} 开始写入长期记忆，待处理 {count} 条"
    if event == "context.embedding.sync.done":
        count = payload.get("count") or 0
        return f"{model or '记忆写入模型'} 完成长记忆写入，共 {count} 条"
    if event == "context.assembled":
        return "本轮对话上下文已整理完成"
    if event == "semantic.plan.fast_route":
        route = payload.get("route") or "快速路径"
        return f"命中快速处理路径: {route}"
    if event == "semantic.plan":
        intent = _translate_intent(payload.get("intent"))
        return f"{model or '语言模型'} 已判断本轮需求类型: {intent}"
    if event == "tool.plan":
        calls = payload.get("calls") or []
        return f"{model or '工具规划模型'} 已规划工具调用，共 {len(calls)} 个"
    if event == "tool.result":
        results = payload.get("results") or []
        ok = sum(1 for item in results if isinstance(item, dict) and item.get("ok"))
        return f"工具执行完成，成功 {ok}/{len(results)}"
    if event == "vision.observe.qq.pending":
        return "收到 QQ 图片，已转入后台识别"
    if event == "vision.observe.qq.pending_query":
        return "识别到用户正在追问刚发的图片，先给出稍后回复提示"
    if event == "chat.preface_ack":
        return "识别到用户是在为图片做铺垫，先接住这句话"
    if event == "vision.observe.qq":
        return f"{model or '视觉模型'} 完成 QQ 图片首轮识别"
    if event == "vision.observe.qq.preface_followup":
        return "上一轮图片预告已接住，并完成主动回复"
    if event == "vision.observe.qq.followup":
        return "图片首轮识别完成，挂起追问也已补回给 QQ"
    if event == "vision.observe.qq.context_followup":
        return "图片识别完成，已承接上一句语境主动回复"
    if event == "vision.reply.relevance":
        score = payload.get("score") or 0
        if payload.get("should_reply"):
            decision = "已决定主动回复图片"
        elif payload.get("fallback_text_only"):
            decision = "图片关联不够强，改为普通文本回复"
        else:
            decision = "图片先记入记忆，暂不主动回复"
        return f"{model or '语言模型'} 完成图片关联判断 {score}/100，{decision}"
    if event == "vision.observe":
        return f"{model or '视觉模型'} 完成 ESP32 画面观察"
    if event == "audio.capture.start":
        return "ESP32 开始上传麦克风录音"
    if event == "audio.capture.chunk":
        kb = int(payload.get("bytes_received", 0)) // 1024
        return f"ESP32 录音上传中，已接收约 {kb} KB"
    if event == "audio.capture.saved":
        peak = int(payload.get("audio_peak") or 0)
        nonzero_ratio = float(payload.get("audio_nonzero_ratio") or 0.0)
        if peak or nonzero_ratio:
            return f"ESP32 录音已保存为 WAV 文件，峰值 {peak}，非零占比 {nonzero_ratio * 100:.2f}%"
        return "ESP32 录音已保存为 WAV 文件"
    if event == "audio.asr.start":
        return f"{model or '语音识别模型'} 开始转写 ESP32 录音"
    if event == "audio.asr.done":
        transcript = _short_text(payload.get("transcript") or "", 80)
        return f"{model or '语音识别模型'} 完成转写: {transcript}"
    if event == "audio.asr.error":
        return f"ESP32 语音识别失败: {_humanize_error_text(payload.get('error'))}"
    if event == "root.command.start":
        command = _short_text(payload.get("command") or "", 50)
        return f"开始执行高级指令: {command}"
    if event == "root.command.done":
        return "高级指令执行完成，结果已回传"
    if event == "chat.record":
        reply = _short_text(payload.get("assistant_text") or "", 120)
        return f"{model or '语言模型'} 完成回复: {reply}"
    if event == "chat.reply.empty_fallback":
        return "模型本轮没有产出内容，已自动切换到兜底回复"
    if event == "chat.outgoing.qq.empty_guard":
        return "准备发送到 QQ 的文本被清洗成空白，已自动换成兜底提示"
    if event == "chat.outgoing.qq":
        action = payload.get("action") or "send_msg"
        return f"QQ 下行动作已投递给 NapCat: {action}"
    if event == "qq.action.response":
        retcode = payload.get("retcode")
        return f"NapCat 动作响应正常，返回码 {retcode if retcode not in (None, '') else 0}"
    if event == "qq.action.error":
        retcode = payload.get("retcode")
        wording = _short_text(payload.get("wording") or "", 80)
        if wording:
            return f"NapCat 动作响应异常，返回码 {retcode}: {wording}"
        return f"NapCat 动作响应异常，返回码 {retcode}"
    if event == "chat.outgoing.esp32.done":
        return "ESP32 文本回复已发送完成"
    if event == "task.start":
        return f"后台任务启动: {_translate_task_type(payload.get('task_type'))}"
    if event == "task.done":
        return f"后台任务完成: {_translate_task_type(payload.get('task_type'))}"
    if event == "worker.state":
        worker = _translate_worker_name(payload.get("worker"))
        state = _translate_worker_state(payload.get("state"))
        pending = payload.get("pending", 0)
        return f"{worker} {state}，当前排队 {pending}"
    if event == "approval.pending":
        action = _translate_approval_action(payload.get("action_type"))
        return f"高风险操作待审批: {action} ({payload.get('approval_id')})"
    if event == "approval.approved":
        action = _translate_approval_action(payload.get("action_type"))
        return f"高风险操作已批准: {action} ({payload.get('approval_id')})"
    if event == "approval.rejected":
        action = _translate_approval_action(payload.get("action_type"))
        return f"高风险操作已拒绝: {action} ({payload.get('approval_id')})"
    if event == "memory.maintenance.done":
        return "滚动记忆总结已完成"
    if event == "memory.maintenance.skip":
        return "检查了滚动记忆窗口，当前不需要更新"
    if event == "memory.structured.start":
        entry_count = int(payload.get("entry_count") or 0)
        return f"{model or '结构化记忆模型'} 开始提炼结构化记忆，窗口 {entry_count} 条"
    if event == "memory.structured.done":
        memories_saved = int(payload.get("memories_saved") or 0)
        profiles_saved = int(payload.get("profiles_saved") or 0)
        if memories_saved <= 0 and profiles_saved <= 0:
            return "结构化记忆抽取完成，但没有新增会话记忆或用户画像"
        return f"结构化记忆已更新，会话记忆 {memories_saved} 条，画像 {profiles_saved} 条"
    if event == "memory.structured.error":
        return f"结构化记忆更新失败: {_humanize_error_text(payload.get('error'))}"
    if event == "temporal.offline_gap.start":
        gap = float(payload.get("gap_minutes") or 0.0)
        return f"检测到对话离线 {gap:.1f} 分钟，开始做回归时段分析"
    if event == "temporal.offline_gap.done":
        gap = float(payload.get("gap_minutes") or 0.0)
        summary = _short_text(payload.get("summary") or "", 70)
        if summary:
            return f"离线 {gap:.1f} 分钟分析完成: {summary}"
        return f"离线 {gap:.1f} 分钟分析完成"
    if event == "temporal.offline_gap.error":
        return f"离线时段分析失败: {_humanize_error_text(payload.get('error'))}"
    if event.endswith(".error") or event == "chat.error":
        return f"{_translate_event_label(event)}失败: {_humanize_error_text(payload.get('error') or payload.get('text'))}"
    return f"{event} 已记录"


class TraceConsoleFilter(logging.Filter):
    QUIET_EVENTS = {
        "assistant.delta",
    }

    def filter(self, record: logging.LogRecord) -> bool:
        try:
            payload = json.loads(record.getMessage())
        except Exception:
            return True
        event = str(payload.get("event") or "")
        if event in self.QUIET_EVENTS:
            return False
        if event == "worker.state" and payload.get("worker") == "memory_maintenance":
            state = str(payload.get("state") or "")
            pending = int(payload.get("pending") or 0)
            if state == "reused" and pending <= 0:
                return False
        return True


class TraceConsoleFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        try:
            payload = json.loads(record.getMessage())
        except Exception:
            return super().format(record)
        event = str(payload.get("event") or "")
        text = _render_trace_event(event, payload)
        if not text:
            return ""
        return f"{self.formatTime(record, self.datefmt)} {text}"



class TraceFileFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        try:
            payload = json.loads(record.getMessage())
        except Exception:
            return super().format(record)
        event = str(payload.get("event") or "")
        summary = _render_trace_event(event, payload)
        details = _build_trace_details(event, payload)
        if details:
            return f"{self.formatTime(record, self.datefmt)} {summary} | {details}"
        return f"{self.formatTime(record, self.datefmt)} {summary}"


LOG_CATEGORY_LABELS = {
    "all": "全部",
    "system": "系统",
    "chat": "对话",
    "audio": "语音",
    "vision": "图片",
    "memory": "记忆",
    "tools": "工具",
    "device": "设备",
    "other": "其他",
}
LOG_TIMESTAMP_RE = re.compile(r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3})\s+(?P<body>.*)$")
QQ_ACTIVITY_TIMEOUT_SECONDS = 300.0
ESP32_ACTIVITY_TIMEOUT_SECONDS = 45.0


def _tail_lines(path, limit: int) -> list[str]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        return list(deque((line.rstrip("\r\n") for line in handle), maxlen=max(1, limit)))


def _split_log_line(line: str) -> tuple[str, str]:
    match = LOG_TIMESTAMP_RE.match(line.strip())
    if not match:
        return "", line.strip()
    return match.group("ts"), match.group("body").strip()


def _guess_log_category(text: str) -> str:
    lowered = text.lower()
    if any(token in lowered for token in ("uvicorn", "启动并行异构控制脑", "application startup", "模型路由", "运行日志:", "模型轨迹/聊天记录日志")):
        return "system"
    if any(token in lowered for token in ("esp32 telemetry", "状态上报", "ws_connected", "终端已连接", "终端断开", "free_heap", "wifi_rssi")):
        return "device"
    if any(token in lowered for token in ("audio", "asr", "tts", "wav", "pcm", "语音", "录音", "麦克风", "转写")):
        return "audio"
    if any(token in lowered for token in ("offline gap", "离线", "回归时段分析", "时间断层", "temporal.offline_gap")):
        return "memory"
    if any(token in lowered for token in ("vision", "图片", "图像", "视觉", "frame", "napcat.image")):
        return "vision"
    if any(token in lowered for token in ("memory", "embedding", "subconscious", "记忆", "总结", "summary")):
        return "memory"
    if any(token in lowered for token in ("tool", "approval", "root", "mcp facade", "外挂接口", "审批", "高级指令")):
        return "tools"
    if any(token in lowered for token in ("chat", "qq 消息", "qq 下行", "收到消息", "完成回复", "对话", "回复", "napcatqq", "napcat 动作")):
        return "chat"
    return "other"


def _collect_log_entries(*, limit_per_file: int = 160, category: str = "all", search: str = "") -> dict[str, Any]:
    files = {
        "runtime": config.runtime_log_file,
        "trace": config.trace_log_file,
    }
    search_text = search.strip().lower()
    counts = {name: 0 for name in LOG_CATEGORY_LABELS if name != "all"}
    entries: list[dict[str, Any]] = []
    for source, path in files.items():
        for line in _tail_lines(path, limit_per_file):
            timestamp, body = _split_log_line(line)
            module = _guess_log_category(body or line)
            counts[module] = counts.get(module, 0) + 1
            haystack = f"{source} {line}".lower()
            if category != "all" and module != category:
                continue
            if search_text and search_text not in haystack:
                continue
            entries.append(
                {
                    "timestamp": timestamp,
                    "source": source,
                    "category": module,
                    "message": body or line,
                }
            )
    entries.sort(key=lambda item: (item.get("timestamp") or "", item.get("source") or ""), reverse=True)
    return {
        "entries": entries[: max(20, min(limit_per_file * 2, 600))],
        "counts": counts,
        "files": {name: str(path) for name, path in files.items()},
    }


def _build_waveform_svg(samples: list[int], *, width: int = 960, height: int = 220) -> str:
    if not samples:
        return (
            f'<svg viewBox="0 0 {width} {height}" xmlns="http://www.w3.org/2000/svg">'
            '<rect width="100%" height="100%" fill="#fffaf2"/>'
            f'<line x1="0" y1="{height/2:.1f}" x2="{width}" y2="{height/2:.1f}" stroke="#c8b9a3" stroke-dasharray="6 6"/>'
            '<text x="50%" y="50%" dominant-baseline="middle" text-anchor="middle" fill="#8b7355" font-size="16">没有可用波形</text>'
            "</svg>"
        )
    max_amp = max(max(abs(v) for v in samples), 1)
    center_y = height / 2
    scale_y = (height * 0.42) / max_amp
    step_x = width / max(len(samples) - 1, 1)
    points = " ".join(
        f"{idx * step_x:.2f},{center_y - (sample * scale_y):.2f}"
        for idx, sample in enumerate(samples)
    )
    return (
        f'<svg viewBox="0 0 {width} {height}" xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="none">'
        '<defs><linearGradient id="waveGrad" x1="0" y1="0" x2="1" y2="0">'
        '<stop offset="0%" stop-color="#0f766e"/><stop offset="100%" stop-color="#c2410c"/></linearGradient></defs>'
        '<rect width="100%" height="100%" fill="#fffaf2"/>'
        f'<line x1="0" y1="{center_y:.2f}" x2="{width}" y2="{center_y:.2f}" stroke="#d8cfbf" stroke-dasharray="6 6"/>'
        f'<polyline fill="none" stroke="url(#waveGrad)" stroke-width="2.5" points="{points}"/>'
        "</svg>"
    )


def _sample_waveform(path: Path, *, buckets: int = 240) -> tuple[list[int], dict[str, Any]]:
    with wave.open(str(path), "rb") as wav_file:
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        frame_rate = wav_file.getframerate()
        frame_count = wav_file.getnframes()
        raw_frames = wav_file.readframes(frame_count)
    if sample_width != 2:
        raise ValueError(f"仅支持 16-bit WAV，当前为 {sample_width * 8}-bit")
    frame_stride = max(1, channels)
    values: list[int] = []
    for offset in range(0, len(raw_frames), sample_width * frame_stride):
        values.append(int.from_bytes(raw_frames[offset : offset + sample_width], "little", signed=True))
    if not values:
        return [], {
            "channels": channels,
            "sample_width": sample_width,
            "frame_rate": frame_rate,
            "frame_count": frame_count,
            "duration_s": 0.0,
        }
    bucket_size = max(1, len(values) // buckets)
    sampled: list[int] = []
    for index in range(0, len(values), bucket_size):
        window = values[index : index + bucket_size]
        if not window:
            continue
        max_sample = max(window)
        min_sample = min(window)
        sampled.append(max_sample if abs(max_sample) >= abs(min_sample) else min_sample)
        if len(sampled) >= buckets:
            break
    return sampled, {
        "channels": channels,
        "sample_width": sample_width,
        "frame_rate": frame_rate,
        "frame_count": frame_count,
        "duration_s": round(frame_count / frame_rate, 3) if frame_rate else 0.0,
    }


def _build_audio_waveform_entry(path: Path, *, audio_type: str, audio_label: str) -> dict[str, Any]:
    try:
        sampled, info = _sample_waveform(path)
        audio_stats = _analyze_wav_audio(path.read_bytes())
        svg = _build_waveform_svg(sampled)
        size = path.stat().st_size
        return {
            "name": path.name,
            "type": audio_type,
            "type_label": audio_label,
            "path": str(path),
            "size_bytes": size,
            "last_modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(path.stat().st_mtime)),
            "mtime": path.stat().st_mtime,
            "channels": info["channels"],
            "sample_width_bits": int(info["sample_width"]) * 8,
            "sample_rate": info["frame_rate"],
            "frame_count": info["frame_count"],
            "duration_s": info["duration_s"],
            "audio_peak": int(audio_stats.get("peak") or 0),
            "audio_rms": round(float(audio_stats.get("rms") or 0.0), 2),
            "audio_nonzero_ratio_percent": round(float(audio_stats.get("nonzero_ratio") or 0.0) * 100, 3),
            "svg": svg,
        }
    except Exception as exc:
        return {
            "name": path.name,
            "type": audio_type,
            "type_label": audio_label,
            "path": str(path),
            "size_bytes": path.stat().st_size if path.exists() else 0,
            "last_modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(path.stat().st_mtime)),
            "mtime": path.stat().st_mtime if path.exists() else 0.0,
            "error": str(exc),
            "svg": _build_waveform_svg([]),
        }


def _collect_recent_audio_waveforms(limit: int = 8, search: str = "") -> dict[str, Any]:
    audio_dirs = [
        (config.data_dir / "esp32_audio", "recording", "ESP32 录音"),
        (config.data_dir / "esp32_tts", "tts", "TTS 播报"),
    ]
    entries: list[dict[str, Any]] = []
    search_text = search.strip().lower()
    per_dir_limit = max(1, limit)
    for audio_dir, audio_type, audio_label in audio_dirs:
        if not audio_dir.exists():
            continue
        files = sorted(audio_dir.glob("*.wav"), key=lambda item: item.stat().st_mtime, reverse=True)
        if audio_type == "tts":
            timestamped = [item for item in files if item.name != "last_tts.wav"]
            files = timestamped or files
        if search_text:
            files = [
                item
                for item in files
                if search_text in item.name.lower() or search_text in audio_label.lower() or search_text in audio_type.lower()
            ]
        files = files[:per_dir_limit]
        for path in files:
            entries.append(_build_audio_waveform_entry(path, audio_type=audio_type, audio_label=audio_label))
    entries.sort(key=lambda item: float(item.get("mtime") or 0.0), reverse=True)
    entries = entries[: max(1, limit)]
    return {
        "entries": entries,
        "audio_dir": str(config.data_dir / "esp32_audio"),
        "tts_dir": str(config.data_dir / "esp32_tts"),
    }


def _build_logs_page() -> str:
    category_buttons = "".join(
        f'<button class="chip" data-category="{key}">{label}</button>'
        for key, label in LOG_CATEGORY_LABELS.items()
    )
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MCP-Robot 日志面板</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f5f1e8;
      --panel: #fffdf8;
      --line: #d8cfbf;
      --text: #2b241c;
      --muted: #786a59;
      --accent: #0f766e;
      --accent-soft: #d7f2ee;
      --warn: #b45309;
      --danger: #b91c1c;
      --shadow: 0 10px 30px rgba(52, 38, 22, 0.08);
      font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    }}
    body {{
      margin: 0;
      background: radial-gradient(circle at top, #fff8ee 0%, var(--bg) 50%, #efe7db 100%);
      color: var(--text);
    }}
    .wrap {{
      max-width: 1280px;
      margin: 0 auto;
      padding: 24px;
    }}
    .hero {{
      background: linear-gradient(135deg, rgba(15,118,110,.12), rgba(180,83,9,.12));
      border: 1px solid rgba(15,118,110,.15);
      border-radius: 20px;
      padding: 24px;
      box-shadow: var(--shadow);
    }}
    .nav {{
      display: flex;
      gap: 10px;
      margin-top: 14px;
      flex-wrap: wrap;
    }}
    .nav a {{
      display: inline-flex;
      align-items: center;
      gap: 8px;
      border-radius: 999px;
      padding: 9px 14px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,.8);
      color: var(--text);
      text-decoration: none;
      font-size: 13px;
      font-weight: 600;
    }}
    .nav a.active {{
      background: var(--accent);
      color: white;
      border-color: var(--accent);
    }}
    h1 {{
      margin: 0 0 8px;
      font-size: 28px;
    }}
    .sub {{
      color: var(--muted);
      font-size: 14px;
    }}
    .toolbar {{
      display: grid;
      grid-template-columns: 1fr auto auto;
      gap: 12px;
      margin-top: 18px;
    }}
    input, select {{
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--text);
      border-radius: 12px;
      padding: 12px 14px;
      font-size: 14px;
    }}
    .chips {{
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      margin-top: 16px;
    }}
    .chip {{
      border: 1px solid var(--line);
      background: rgba(255,255,255,.8);
      color: var(--text);
      border-radius: 999px;
      padding: 8px 12px;
      cursor: pointer;
      font-size: 13px;
    }}
    .chip.active {{
      background: var(--accent);
      border-color: var(--accent);
      color: white;
    }}
    .meta {{
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 12px;
      margin-top: 18px;
    }}
    .card {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 14px 16px;
      box-shadow: var(--shadow);
    }}
    .card b {{
      display: block;
      margin-bottom: 6px;
    }}
    .grid {{
      display: grid;
      grid-template-columns: minmax(0, 2fr) minmax(280px, 1fr);
      gap: 16px;
      margin-top: 18px;
    }}
    .panel {{
      background: rgba(255,255,255,.75);
      backdrop-filter: blur(8px);
      border: 1px solid var(--line);
      border-radius: 18px;
      overflow: hidden;
      box-shadow: var(--shadow);
    }}
    .panel header {{
      padding: 16px 18px;
      border-bottom: 1px solid var(--line);
      font-weight: 700;
    }}
    .log-list {{
      max-height: 70vh;
      overflow: auto;
    }}
    .log-item {{
      padding: 14px 18px;
      border-bottom: 1px solid rgba(216, 207, 191, .7);
    }}
    .log-item:last-child {{
      border-bottom: 0;
    }}
    .log-top {{
      display: flex;
      gap: 10px;
      align-items: center;
      font-size: 12px;
      color: var(--muted);
      margin-bottom: 6px;
    }}
    .badge {{
      border-radius: 999px;
      padding: 2px 8px;
      background: var(--accent-soft);
      color: var(--accent);
      font-weight: 700;
    }}
    .msg {{
      white-space: pre-wrap;
      word-break: break-word;
      line-height: 1.5;
      font-size: 14px;
    }}
    .counts {{
      display: grid;
      gap: 10px;
      padding: 16px 18px;
    }}
    .count-row {{
      display: flex;
      justify-content: space-between;
      gap: 12px;
      font-size: 14px;
    }}
    .muted {{
      color: var(--muted);
    }}
    @media (max-width: 900px) {{
      .toolbar, .meta, .grid {{
        grid-template-columns: 1fr;
      }}
      .log-list {{
        max-height: none;
      }}
    }}
  </style>
</head>
<body>
  <div class="wrap">
    <section class="hero">
      <h1>日志分拣面板</h1>
      <div class="sub">把运行日志和轨迹日志按模块拆开看，盯 ASR、图片、记忆、工具问题会轻松很多。</div>
      <div class="nav">
        <a class="active" href="/logs">分类日志</a>
        <a href="/logs/audio">音频波形</a>
      </div>
      <div class="toolbar">
        <input id="search" type="text" placeholder="搜索关键词，比如 ASR、图片、memory、approval">
        <select id="limit">
          <option value="120">每个文件最近 120 行</option>
          <option value="200" selected>每个文件最近 200 行</option>
          <option value="400">每个文件最近 400 行</option>
        </select>
        <select id="refresh">
          <option value="0">手动刷新</option>
          <option value="5" selected>每 5 秒刷新</option>
          <option value="10">每 10 秒刷新</option>
        </select>
      </div>
      <div class="chips" id="chips">{category_buttons}</div>
      <div class="meta">
        <div class="card"><b>健康状态</b><span id="health">加载中</span></div>
        <div class="card"><b>语言 / ASR / TTS</b><span id="models">加载中</span></div>
        <div class="card"><b>结构化记忆</b><span id="memory-status">加载中</span></div>
        <div class="card"><b>运行日志</b><span class="muted">{escape(str(config.runtime_log_file))}</span></div>
        <div class="card"><b>轨迹日志</b><span class="muted">{escape(str(config.trace_log_file))}</span></div>
      </div>
    </section>
    <section class="grid">
      <div class="panel">
        <header id="panel-title">日志列表</header>
        <div class="log-list" id="log-list"></div>
      </div>
      <div class="panel">
        <header>分类计数</header>
        <div class="counts" id="counts"></div>
      </div>
    </section>
  </div>
  <script>
    const labels = {json.dumps(LOG_CATEGORY_LABELS, ensure_ascii=False)};
    let currentCategory = 'all';
    let timer = null;

    function escapeHtml(value) {{
      return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
    }}

    function setActiveChip() {{
      document.querySelectorAll('.chip').forEach((button) => {{
        button.classList.toggle('active', button.dataset.category === currentCategory);
      }});
    }}

    function renderCounts(counts) {{
      const root = document.getElementById('counts');
      const rows = Object.entries(labels)
        .filter(([key]) => key !== 'all')
        .map(([key, label]) => {{
          const value = counts[key] || 0;
          return `<div class="count-row"><span>${{label}}</span><b>${{value}}</b></div>`;
        }})
        .join('');
      root.innerHTML = rows;
    }}

    function renderEntries(entries) {{
      const root = document.getElementById('log-list');
      if (!entries.length) {{
        root.innerHTML = '<div class="log-item muted">当前筛选条件下没有日志。</div>';
        return;
      }}
      root.innerHTML = entries.map((entry) => `
        <div class="log-item">
          <div class="log-top">
            <span>${{escapeHtml(entry.timestamp || '无时间戳')}}</span>
            <span class="badge">${{escapeHtml(labels[entry.category] || entry.category)}}</span>
            <span>${{escapeHtml(entry.source)}}</span>
          </div>
          <div class="msg">${{escapeHtml(entry.message)}}</div>
        </div>
      `).join('');
    }}

    async function refreshLogs() {{
      const search = document.getElementById('search').value.trim();
      const limit = document.getElementById('limit').value;
      const query = new URLSearchParams({{ category: currentCategory, search, limit }});
      const response = await fetch(`/api/logs?${{query.toString()}}`, {{ cache: 'no-store' }});
      const data = await response.json();
      document.getElementById('panel-title').textContent = `日志列表 · ${{labels[currentCategory] || currentCategory}}`;
      const qqAge = data.health.qq_last_activity_age_s == null ? '无' : `${{Number(data.health.qq_last_activity_age_s).toFixed(1)}}s`;
      const espAge = data.health.esp32_last_activity_age_s == null ? '无' : `${{Number(data.health.esp32_last_activity_age_s).toFixed(1)}}s`;
      document.getElementById('health').textContent = data.health.ok
        ? `在线 · QQ=${{data.health.qq_connected ? '活跃' : '离线'}}(${{qqAge}}) · ESP32=${{data.health.esp32_connected ? '活跃' : '离线'}}(${{espAge}})`
        : '异常';
      document.getElementById('models').textContent = `QQ文本=${{data.health.models.qq_language}} / 默认=${{data.health.models.default_language}} / ASR=${{data.health.models.asr_provider}}:${{data.health.models.asr}} / ESP32语音=${{data.health.models.esp32_voice}} / TTS=${{data.health.models.tts_provider}}:${{data.health.models.tts}}`;
      const memoryHealth = data.health.structured_memory || {{}};
      document.getElementById('memory-status').textContent =
        `会话记忆=${{memoryHealth.memory_count ?? 0}} 条 / 画像=${{memoryHealth.profile_count ?? 0}} 条`;
      renderCounts(data.counts || {{}});
      renderEntries(data.entries || []);
    }}

    function resetTimer() {{
      if (timer) {{
        clearInterval(timer);
        timer = null;
      }}
      const every = Number(document.getElementById('refresh').value || '0');
      if (every > 0) {{
        timer = setInterval(refreshLogs, every * 1000);
      }}
    }}

    document.getElementById('search').addEventListener('change', refreshLogs);
    document.getElementById('limit').addEventListener('change', refreshLogs);
    document.getElementById('refresh').addEventListener('change', () => {{
      resetTimer();
      refreshLogs();
    }});
    document.querySelectorAll('.chip').forEach((button) => {{
      button.addEventListener('click', () => {{
        currentCategory = button.dataset.category;
        setActiveChip();
        refreshLogs();
      }});
    }});
    setActiveChip();
    resetTimer();
    refreshLogs();
  </script>
</body>
</html>"""


def _build_audio_logs_page() -> str:
    return """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MCP-Robot 音频波形</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f5f1e8;
      --panel: #fffdf8;
      --line: #d8cfbf;
      --text: #2b241c;
      --muted: #786a59;
      --accent: #0f766e;
      --accent-soft: #d7f2ee;
      --shadow: 0 10px 30px rgba(52, 38, 22, 0.08);
      font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    }
    body {
      margin: 0;
      background: radial-gradient(circle at top, #fff8ee 0%, var(--bg) 50%, #efe7db 100%);
      color: var(--text);
    }
    .wrap {
      max-width: 1320px;
      margin: 0 auto;
      padding: 24px;
    }
    .hero {
      background: linear-gradient(135deg, rgba(15,118,110,.12), rgba(180,83,9,.12));
      border: 1px solid rgba(15,118,110,.15);
      border-radius: 20px;
      padding: 24px;
      box-shadow: var(--shadow);
    }
    .nav {
      display: flex;
      gap: 10px;
      margin-top: 14px;
      flex-wrap: wrap;
    }
    .nav a {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      border-radius: 999px;
      padding: 9px 14px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,.8);
      color: var(--text);
      text-decoration: none;
      font-size: 13px;
      font-weight: 600;
    }
    .nav a.active {
      background: var(--accent);
      color: white;
      border-color: var(--accent);
    }
    h1 {
      margin: 0 0 8px;
      font-size: 28px;
    }
    .sub {
      color: var(--muted);
      font-size: 14px;
    }
    .toolbar {
      display: grid;
      grid-template-columns: 1fr auto auto;
      gap: 12px;
      margin-top: 18px;
    }
    .meta {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 12px;
      margin-top: 18px;
    }
    input, select {
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--text);
      border-radius: 12px;
      padding: 12px 14px;
      font-size: 14px;
    }
    .card {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 14px 16px;
      box-shadow: var(--shadow);
    }
    .card b {
      display: block;
      margin-bottom: 6px;
    }
    .audio-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
      gap: 16px;
      margin-top: 18px;
    }
    .audio-card {
      background: rgba(255,255,255,.8);
      border: 1px solid var(--line);
      border-radius: 18px;
      box-shadow: var(--shadow);
      overflow: hidden;
    }
    .audio-head {
      padding: 16px 18px 12px;
      border-bottom: 1px solid rgba(216, 207, 191, .75);
    }
    .audio-name {
      font-weight: 700;
      font-size: 15px;
      word-break: break-all;
    }
    .audio-meta {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
      padding: 14px 18px;
      font-size: 13px;
      color: var(--muted);
    }
    .audio-wave {
      padding: 0 18px 18px;
    }
    .audio-wave svg {
      display: block;
      width: 100%;
      height: 220px;
      border-radius: 14px;
      border: 1px solid rgba(216, 207, 191, .8);
      background: #fffaf2;
    }
    .muted {
      color: var(--muted);
    }
    .error {
      color: #b91c1c;
      font-size: 13px;
    }
    @media (max-width: 900px) {
      .toolbar, .meta {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <section class="hero">
      <h1>音频波形分区</h1>
      <div class="sub">查看最近保存下来的 ESP32 录音和 TTS 播报 WAV。平线通常说明 PCM 近乎静音。</div>
      <div class="nav">
        <a href="/logs">分类日志</a>
        <a class="active" href="/logs/audio">音频波形</a>
      </div>
      <div class="toolbar">
        <input id="search" type="text" placeholder="按文件名或类型搜索，比如 tts、录音、29935">
        <select id="limit">
          <option value="4">最近 4 段</option>
          <option value="8" selected>最近 8 段</option>
          <option value="12">最近 12 段</option>
        </select>
        <select id="refresh">
          <option value="0">手动刷新</option>
          <option value="5" selected>每 5 秒刷新</option>
          <option value="10">每 10 秒刷新</option>
        </select>
      </div>
      <div class="meta">
        <div class="card"><b>健康状态</b><span id="health">加载中</span></div>
        <div class="card"><b>语音模型</b><span id="models">加载中</span></div>
        <div class="card"><b>音频目录</b><span id="audio-dir" class="muted">加载中</span></div>
      </div>
    </section>
    <section class="audio-grid" id="audio-grid"></section>
  </div>
  <script>
    let timer = null;
    function escapeHtml(value) {
      return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
    }
    function resetTimer() {
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
      const every = Number(document.getElementById('refresh').value || '0');
      if (every > 0) {
        timer = setInterval(refreshAudio, every * 1000);
      }
    }
    function renderCards(entries) {
      const root = document.getElementById('audio-grid');
      if (!entries.length) {
        root.innerHTML = '<div class="card muted">当前没有找到 WAV 音频文件。</div>';
        return;
      }
      root.innerHTML = entries.map((entry) => `
        <article class="audio-card">
          <div class="audio-head">
            <div class="audio-name">[${escapeHtml(entry.type_label || entry.type || '音频')}] ${escapeHtml(entry.name)}</div>
            <div class="muted">${escapeHtml(entry.last_modified || '')}</div>
          </div>
          <div class="audio-meta">
            <div><b>时长</b><div>${escapeHtml(entry.duration_s)} s</div></div>
            <div><b>采样率</b><div>${escapeHtml(entry.sample_rate || '-')} Hz</div></div>
            <div><b>声道 / 位宽</b><div>${escapeHtml(entry.channels || '-')} ch / ${escapeHtml(entry.sample_width_bits || '-')} bit</div></div>
            <div><b>文件大小</b><div>${escapeHtml(entry.size_bytes || 0)} bytes</div></div>
            <div><b>峰值</b><div>${escapeHtml(entry.audio_peak ?? '-')}</div></div>
            <div><b>非零占比</b><div>${escapeHtml(entry.audio_nonzero_ratio_percent ?? '-')}%</div></div>
            <div><b>RMS</b><div>${escapeHtml(entry.audio_rms ?? '-')}</div></div>
            <div><b>类型</b><div>${escapeHtml(entry.type_label || entry.type || '-')}</div></div>
            <div><b>路径</b><div class="muted">${escapeHtml(entry.path || '')}</div></div>
          </div>
          <div class="audio-wave">${entry.svg || ''}</div>
          ${entry.error ? `<div class="audio-meta"><div class="error">读取失败: ${escapeHtml(entry.error)}</div></div>` : ''}
        </article>
      `).join('');
    }
    async function refreshAudio() {
      const search = document.getElementById('search').value.trim();
      const limit = document.getElementById('limit').value;
      const query = new URLSearchParams({ limit, search });
      const response = await fetch(`/api/audio-waveforms?${query.toString()}`, { cache: 'no-store' });
      const data = await response.json();
      const qqAge = data.health.qq_last_activity_age_s == null ? '无' : `${Number(data.health.qq_last_activity_age_s).toFixed(1)}s`;
      const espAge = data.health.esp32_last_activity_age_s == null ? '无' : `${Number(data.health.esp32_last_activity_age_s).toFixed(1)}s`;
      document.getElementById('health').textContent = `QQ=${data.health.qq_connected ? '活跃' : '离线'}(${qqAge}) · ESP32=${data.health.esp32_connected ? '活跃' : '离线'}(${espAge})`;
      document.getElementById('models').textContent = `ASR=${data.health.models.asr_provider}:${data.health.models.asr} / ESP32语音=${data.health.models.esp32_voice} / TTS=${data.health.models.tts_provider}:${data.health.models.tts}`;
      document.getElementById('audio-dir').textContent = `录音=${data.audio_dir || ''} · TTS=${data.tts_dir || ''}`;
      renderCards(data.entries || []);
    }
    document.getElementById('search').addEventListener('change', refreshAudio);
    document.getElementById('limit').addEventListener('change', refreshAudio);
    document.getElementById('refresh').addEventListener('change', () => {
      resetTimer();
      refreshAudio();
    });
    resetTimer();
    refreshAudio();
  </script>
</body>
</html>"""


log_format = "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
console_handler = logging.StreamHandler()
console_handler.setFormatter(logging.Formatter(log_format))
runtime_file_handler = RotatingFileHandler(
    config.runtime_log_file,
    maxBytes=2 * 1024 * 1024,
    backupCount=3,
    encoding="utf-8",
)
runtime_file_handler.setFormatter(logging.Formatter(log_format))

root_logger = logging.getLogger()
root_logger.setLevel(logging.INFO)
root_logger.handlers.clear()
root_logger.addHandler(console_handler)
root_logger.addHandler(runtime_file_handler)
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)
logging.getLogger("uvicorn.access").setLevel(logging.WARNING)

trace_handler = RotatingFileHandler(
    config.trace_log_file,
    maxBytes=4 * 1024 * 1024,
    backupCount=5,
    encoding="utf-8",
)
trace_handler.setFormatter(TraceFileFormatter())
trace_console_handler = logging.StreamHandler()
trace_console_handler.setFormatter(TraceConsoleFormatter())
trace_console_handler.addFilter(TraceConsoleFilter())
trace_logger = logging.getLogger("MCP_Robot.Trace")
trace_logger.setLevel(logging.INFO)
trace_logger.handlers.clear()
trace_logger.addHandler(trace_handler)
trace_logger.addHandler(trace_console_handler)
trace_logger.propagate = False

logger = logging.getLogger("MCP_Robot_Brain")

app = FastAPI(title="云龙虾 (Cloud Lobster) 并行异构控制脑")

ESP32_DISCOVERY_MAGIC = "MCP_ROBOT_DISCOVER_V1"
ESP32_DISCOVERY_PORT = int(os.getenv("ESP32_MCP_DISCOVERY_PORT", "8081"))
_discovery_stop = threading.Event()
_discovery_thread: threading.Thread | None = None


def _local_ip_for_peer(peer_ip: str) -> str:
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect((peer_ip, 9))
        host = probe.getsockname()[0]
        if host and host != "0.0.0.0":
            return host
    finally:
        probe.close()
    return "127.0.0.1"


def _esp32_discovery_endpoint(peer_ip: str) -> str:
    endpoint = os.getenv("ESP32_MCP_DISCOVERY_ENDPOINT", "").strip()
    if endpoint:
        return endpoint
    host = os.getenv("ESP32_MCP_DISCOVERY_HOST", "").strip() or _local_ip_for_peer(peer_ip)
    return f"ws://{host}:8080/esp32_ws"


def _run_esp32_discovery_responder(stop_event: threading.Event) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", ESP32_DISCOVERY_PORT))
        sock.settimeout(1.0)
        logger.info("ESP32 MCP discovery listening on UDP %s", ESP32_DISCOVERY_PORT)
        while not stop_event.is_set():
            try:
                data, addr = sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError as exc:
                if not stop_event.is_set():
                    logger.warning("ESP32 MCP discovery socket error: %s", exc)
                continue
            text = data.decode("utf-8", errors="ignore")
            if ESP32_DISCOVERY_MAGIC not in text:
                continue
            endpoint = _esp32_discovery_endpoint(addr[0])
            payload = json.dumps(
                {
                    "type": "mcp_robot_discovery",
                    "version": 1,
                    "endpoint": endpoint,
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            try:
                sock.sendto(payload, addr)
                logger.info("ESP32 MCP discovery reply: client=%s endpoint=%s", addr[0], endpoint)
            except OSError as exc:
                logger.warning("ESP32 MCP discovery reply failed: %s", exc)
    except OSError as exc:
        logger.warning("ESP32 MCP discovery disabled: %s", exc)
    finally:
        sock.close()


def _start_esp32_discovery_responder() -> None:
    global _discovery_thread
    if ESP32_DISCOVERY_PORT <= 0:
        logger.info("ESP32 MCP discovery disabled by port=%s", ESP32_DISCOVERY_PORT)
        return
    if _discovery_thread and _discovery_thread.is_alive():
        return
    _discovery_stop.clear()
    _discovery_thread = threading.Thread(
        target=_run_esp32_discovery_responder,
        args=(_discovery_stop,),
        name="esp32-mcp-discovery",
        daemon=True,
    )
    _discovery_thread.start()


@app.on_event("startup")
async def _startup_esp32_discovery() -> None:
    _start_esp32_discovery_responder()
    runtime.start_background_workers()


@app.on_event("shutdown")
async def _shutdown_esp32_discovery() -> None:
    _discovery_stop.set()


class ConnectionManager:
    def __init__(self) -> None:
        self.qq_client: WebSocket | None = None
        self.esp32_client: WebSocket | None = None
        self.qq_connected_at: float = 0.0
        self.esp32_connected_at: float = 0.0
        self.qq_last_activity_at: float = 0.0
        self.esp32_last_activity_at: float = 0.0
        self._esp32_tts_chunk_count: int = 0
        self._esp32_tts_b64_chars: int = 0

    def _socket_is_alive(self, websocket: WebSocket | None) -> bool:
        if websocket is None:
            return False
        try:
            return (
                websocket.client_state == WebSocketState.CONNECTED
                and websocket.application_state == WebSocketState.CONNECTED
            )
        except Exception:
            return False

    @staticmethod
    def _age_seconds(last_at: float) -> float | None:
        if last_at <= 0:
            return None
        return max(0.0, time.time() - last_at)

    @property
    def is_qq_connected(self) -> bool:
        age = self._age_seconds(self.qq_last_activity_at)
        return self._socket_is_alive(self.qq_client) and age is not None and age <= QQ_ACTIVITY_TIMEOUT_SECONDS

    @property
    def is_esp32_connected(self) -> bool:
        age = self._age_seconds(self.esp32_last_activity_at)
        return self._socket_is_alive(self.esp32_client) and age is not None and age <= ESP32_ACTIVITY_TIMEOUT_SECONDS

    def note_qq_activity(self) -> None:
        self.qq_last_activity_at = time.time()

    def note_esp32_activity(self, websocket: WebSocket | None = None) -> None:
        if websocket is not None and self.esp32_client is not websocket:
            self.esp32_client = websocket
            if self.esp32_connected_at <= 0:
                self.esp32_connected_at = time.time()
            logger.info("ESP32 终端连接已从上行活动恢复。")
        self.esp32_last_activity_at = time.time()

    def snapshot(self) -> dict[str, Any]:
        return {
            "qq_connected": self.is_qq_connected,
            "esp32_connected": self.is_esp32_connected,
            "qq_last_activity_age_s": self._age_seconds(self.qq_last_activity_at),
            "esp32_last_activity_age_s": self._age_seconds(self.esp32_last_activity_at),
            "qq_socket_alive": self._socket_is_alive(self.qq_client),
            "esp32_socket_alive": self._socket_is_alive(self.esp32_client),
        }

    async def connect_qq(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self.qq_client = websocket
        self.qq_connected_at = time.time()
        self.note_qq_activity()
        logger.info("QQ 终端 (NapCat) 已连接。")

    async def connect_esp32(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self.esp32_client = websocket
        self.esp32_connected_at = time.time()
        self._esp32_tts_chunk_count = 0
        self._esp32_tts_b64_chars = 0
        self.note_esp32_activity()
        logger.info("ESP32 终端已连接。")

    def disconnect_qq(self) -> None:
        self.qq_client = None
        self.qq_connected_at = 0.0
        self.qq_last_activity_at = 0.0
        logger.warning("QQ 终端断开连接。")

    def disconnect_esp32(self, websocket: WebSocket | None = None) -> None:
        if websocket is not None and self.esp32_client is not websocket:
            logger.debug("忽略旧 ESP32 socket 的断开事件。")
            return
        self.esp32_client = None
        self.esp32_connected_at = 0.0
        self.esp32_last_activity_at = 0.0
        self._esp32_tts_chunk_count = 0
        self._esp32_tts_b64_chars = 0
        logger.warning("ESP32 终端断开连接。")

    async def send_to_qq(self, message: dict[str, Any]) -> None:
        if not self._socket_is_alive(self.qq_client):
            logger.warning(
                "QQ 终端未连接，下行动作未送达: action=%s echo=%s",
                message.get("action"),
                message.get("echo"),
            )
            self.disconnect_qq()
            return
        try:
            payload = json.dumps(message, ensure_ascii=False)
            await self.qq_client.send_text(payload)
            self.note_qq_activity()
            logger.info(
                "NapCat 下行动作已发送: action=%s echo=%s bytes=%d",
                message.get("action"),
                message.get("echo"),
                len(payload.encode("utf-8")),
            )
        except Exception:
            logger.exception(
                "NapCat 下行动作发送失败: action=%s echo=%s",
                message.get("action"),
                message.get("echo"),
            )
            self.disconnect_qq()
            raise

    async def send_to_esp32(self, message: dict[str, Any]) -> None:
        if not self._socket_is_alive(self.esp32_client):
            logger.warning("ESP32 未连接，忽略下行消息: %s", message.get("type"))
            self.disconnect_esp32()
            return
        try:
            payload = json.dumps(message, ensure_ascii=False)
            await self.esp32_client.send_text(payload)
            self.note_esp32_activity()
            msg_type = str(message.get("type") or "unknown")
            if msg_type in {"tts_audio_chunk", "tts_pcm_chunk"}:
                audio_b64 = str(message.get("audio_b64") or "")
                self._esp32_tts_chunk_count += 1
                self._esp32_tts_b64_chars += len(audio_b64)
            elif msg_type in {"tts_segment_start", "tts_pcm_start"}:
                self._esp32_tts_chunk_count = 0
                self._esp32_tts_b64_chars = 0
                logger.info("ESP32 下行消息已发送: type=%s bytes=%d", msg_type, len(payload.encode("utf-8")))
            elif msg_type in {"tts_stream_end", "tts_pcm_end"}:
                logger.info(
                    "ESP32 下行 TTS 音频发送完成: chunks=%d chars=%d",
                    self._esp32_tts_chunk_count,
                    self._esp32_tts_b64_chars,
                )
                logger.info("ESP32 下行消息已发送: type=%s bytes=%d", msg_type, len(payload.encode("utf-8")))
                self._esp32_tts_chunk_count = 0
                self._esp32_tts_b64_chars = 0
            else:
                logger.info("ESP32 下行消息已发送: type=%s bytes=%d", msg_type, len(payload.encode("utf-8")))
        except Exception:
            self.disconnect_esp32()
            raise

manager = ConnectionManager()
runtime = RobotRuntime(config, manager)
mcp_facade = None
if config.mcp_facade_enabled:
    try:
        mcp_facade = build_mcp_facade(runtime)
        if mcp_facade is not None:
            app_mount = mcp_facade.streamable_http_app()
        else:
            app_mount = None
    except Exception as exc:
        logger.warning("MCP 外挂接口初始化失败: %s", exc)
        app_mount = None
else:
    app_mount = None


@app.get("/healthz")
async def healthz() -> dict[str, Any]:
    connection = manager.snapshot()
    return {
        "ok": True,
        "qq_connected": connection["qq_connected"],
        "esp32_connected": connection["esp32_connected"],
        "qq_last_activity_age_s": connection["qq_last_activity_age_s"],
        "esp32_last_activity_age_s": connection["esp32_last_activity_age_s"],
        "qq_socket_alive": connection["qq_socket_alive"],
        "esp32_socket_alive": connection["esp32_socket_alive"],
        "models": {
            "language": config.language_model.model,
            "tool": config.tool_model.model,
            "default_language": config.language_model.model,
            "default_tool": config.tool_model.model,
            "qq_language": config.qq_language_model.model,
            "qq_tool": config.qq_tool_model.model,
            "embedding": config.context_embedding.model,
            "vision_low": config.vision_model.model,
            "vision_high": config.vision_highres_model.model,
            "asr": config.asr_model.model,
            "asr_provider": config.asr_model.provider,
            "tts": config.tts_model.model,
            "tts_provider": config.tts_model.provider,
            "tts_voice": config.tts_voice,
            "tts_style_prompt": config.tts_style_prompt,
            "esp32_voice": (
                "doubao-realtime-dialog"
                if runtime.doubao_dialog.available
                else "doubao-dialog-config-missing"
                if config.doubao_dialog.enabled
                else "doubao-dialog-disabled"
            ),
            "doubao_dialog_enabled": config.doubao_dialog.enabled,
            "doubao_dialog_configured": runtime.doubao_dialog.available,
            "doubao_dialog_resource": config.doubao_dialog.resource_id,
            "doubao_dialog_tts_format": config.doubao_dialog.tts_format,
            "doubao_dialog_tts_sample_rate": config.doubao_dialog.tts_sample_rate,
            "doubao_dialog_tts_speaker": config.doubao_dialog.tts_speaker,
        },
        "generic_agent_python": str(config.generic_agent_python),
        "runtime_log": str(config.runtime_log_file),
        "trace_log": str(config.trace_log_file),
        "structured_memory": {
            "memory_file": str(runtime.structured_memory_store.memory_file),
            "profile_file": str(runtime.structured_memory_store.profile_file),
            "memory_count": runtime.structured_memory_store.memory_count(),
            "profile_count": runtime.structured_memory_store.profile_count(),
        },
        "aiot": runtime.proactive_snapshot(),
        "mcp_facade_enabled": bool(app_mount is not None),
        "mcp_mount_path": config.mcp_mount_path,
    }


@app.get("/api/aiot/dashboard")
async def api_aiot_dashboard(refresh: bool = False) -> dict[str, Any]:
    if refresh:
        dashboard = await runtime.refresh_proactive_dashboard()
    else:
        dashboard = runtime.proactive_snapshot()
    return {"ok": True, "dashboard": dashboard}


@app.get("/api/aiot/calendar")
async def api_aiot_calendar() -> dict[str, Any]:
    path = config.aiot_calendar_file
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("[]", encoding="utf-8")
    try:
        events = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"calendar read failed: {exc}") from exc
    return {"ok": True, "file": str(path), "events": events}


@app.post("/api/aiot/calendar")
async def api_aiot_calendar_add(payload: dict[str, Any]) -> dict[str, Any]:
    title = str(payload.get("title") or "").strip()[:60]
    start = str(payload.get("start") or "").strip()[:32]
    if not title or not start:
        raise HTTPException(status_code=400, detail="title and start are required")
    path = config.aiot_calendar_file
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        events = json.loads(path.read_text(encoding="utf-8")) if path.exists() else []
    except Exception:
        events = []
    if not isinstance(events, list):
        events = []
    item = {
        "title": title,
        "start": start,
        "end": str(payload.get("end") or "").strip()[:32],
        "location": str(payload.get("location") or "").strip()[:40],
        "note": str(payload.get("note") or "").strip()[:80],
        "remind_minutes": max(1, min(1440, int(payload.get("remind_minutes") or 20))),
        "enabled": bool(payload.get("enabled", True)),
    }
    events.append(item)
    path.write_text(json.dumps(events, ensure_ascii=False, indent=2), encoding="utf-8")
    dashboard = await runtime.refresh_proactive_dashboard()
    return {"ok": True, "event": item, "dashboard": dashboard}


@app.get("/api/logs")
async def api_logs(category: str = "all", search: str = "", limit: int = 200) -> dict[str, Any]:
    category = category if category in LOG_CATEGORY_LABELS else "all"
    payload = _collect_log_entries(limit_per_file=max(50, min(limit, 500)), category=category, search=search)
    payload["health"] = await healthz()
    return payload


@app.get("/api/audio-waveforms")
async def api_audio_waveforms(limit: int = 8, search: str = "") -> dict[str, Any]:
    payload = _collect_recent_audio_waveforms(limit=max(1, min(limit, 16)), search=search)
    payload["health"] = await healthz()
    return payload


@app.get("/api/esp32/tts-test")
async def api_esp32_tts_test(text: str = "小乐测试语音。") -> dict[str, Any]:
    await runtime.send_esp32_tts_test(text[:120])
    return {"ok": True, "text": text[:120]}


def _select_esp32_replay_wav(file: str = "", min_bytes: int = 32000) -> tuple[Path, int]:
    audio_dir = (config.data_dir / "esp32_audio").resolve()
    wav_path: Path | None = None
    if file.strip():
        candidate = (audio_dir / Path(file).name).resolve()
        if candidate.parent != audio_dir or not candidate.is_file() or candidate.suffix.lower() != ".wav":
            raise HTTPException(status_code=404, detail="ESP32 replay WAV not found")
        wav_path = candidate
    else:
        wavs = sorted(
            (path for path in audio_dir.glob("*.wav") if path.is_file() and path.stat().st_size >= max(0, min_bytes)),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        if wavs:
            wav_path = wavs[0]
    if wav_path is None:
        raise HTTPException(status_code=404, detail="No ESP32 replay WAV available")

    try:
        with wave.open(str(wav_path), "rb") as wav_file:
            rate = wav_file.getframerate()
            frames = wav_file.getnframes()
            duration_ms = int((frames * 1000) / rate) if rate > 0 else 0
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Invalid WAV: {exc}") from exc
    return wav_path, duration_ms


def _parse_diagnostic_detail(detail: str) -> dict[str, Any]:
    parsed: dict[str, Any] = {}
    for item in str(detail or "").split():
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            continue
        try:
            parsed[key] = int(value)
        except ValueError:
            parsed[key] = value
    return parsed


@app.get("/api/esp32/dialog-replay")
async def api_esp32_dialog_replay(file: str = "", min_bytes: int = 32000) -> dict[str, Any]:
    wav_path, duration_ms = _select_esp32_replay_wav(file=file, min_bytes=min_bytes)
    device_id = "ESP32_KORVO_2"
    session_id = f"esp32-dialog-replay-{int(time.time() * 1000)}"
    runtime._track_task(
        runtime._run_esp32_audio_pipeline(
            session_id=session_id,
            device_id=device_id,
            wav_path=wav_path,
            duration_ms=duration_ms,
            audio_source="dialog_replay",
        ),
        task_type="esp32.dialog_replay",
        source="ESP32",
        metadata={"device_id": device_id, "session_id": session_id, "wav": wav_path.name},
    )
    return {
        "ok": True,
        "session_id": session_id,
        "wav": wav_path.name,
        "duration_ms": duration_ms,
    }


@app.get("/api/esp32/barge-tts-diag")
async def api_esp32_barge_tts_diag(
    fmt: str = "RNNM",
    duration_ms: int = 12000,
    file: str = "",
    min_bytes: int = 32000,
    arm_delay_ms: int = 800,
    wait: bool = True,
) -> dict[str, Any]:
    connection = manager.snapshot()
    if not connection["esp32_connected"]:
        raise HTTPException(status_code=409, detail="ESP32 is not connected")

    fmt_clean = re.sub(r"[^A-Za-z]", "", fmt).upper()[:7] or "RNNM"
    if any(ch not in "RMN" for ch in fmt_clean):
        raise HTTPException(status_code=400, detail="fmt must contain only R/M/N channel tokens")
    duration_ms = max(1500, min(12000, int(duration_ms or 12000)))
    arm_delay_ms = max(100, min(3000, int(arm_delay_ms or 800)))

    wav_path, replay_duration_ms = _select_esp32_replay_wav(file=file, min_bytes=min_bytes)
    device_id = "ESP32_KORVO_2"
    command = f"BARGE {fmt_clean} TTS {duration_ms}"
    started_at = time.time()
    await manager.send_to_esp32({"type": "device_command", "command": command})
    await asyncio.sleep(arm_delay_ms / 1000.0)

    session_id = f"esp32-dialog-replay-{int(time.time() * 1000)}"
    runtime._track_task(
        runtime._run_esp32_audio_pipeline(
            session_id=session_id,
            device_id=device_id,
            wav_path=wav_path,
            duration_ms=replay_duration_ms,
            audio_source="dialog_replay",
        ),
        task_type="esp32.barge_tts_diag",
        source="ESP32",
        metadata={
            "device_id": device_id,
            "session_id": session_id,
            "wav": wav_path.name,
            "command": command,
        },
    )

    diagnostic: dict[str, Any] | None = None
    if wait:
        deadline = time.time() + (duration_ms / 1000.0) + 8.0
        while time.time() < deadline:
            diagnostic = runtime.latest_esp32_diagnostic_event(
                name="barge_diag",
                phase="done",
                device_id=device_id,
                after_ts=started_at,
            )
            if diagnostic:
                break
            await asyncio.sleep(0.25)

    diagnostic_fields = _parse_diagnostic_detail(diagnostic.get("detail", "")) if diagnostic else None
    return {
        "ok": diagnostic is not None if wait else True,
        "command": command,
        "session_id": session_id,
        "wav": wav_path.name,
        "replay_duration_ms": replay_duration_ms,
        "diag_duration_ms": duration_ms,
        "diagnostic": diagnostic,
        "diagnostic_fields": diagnostic_fields,
        "note": "Uses Doubao Realtime Dialog TTS playback, not local embedded prompt audio.",
    }


@app.get("/api/esp32/device-command")
async def api_esp32_device_command(command: str) -> dict[str, Any]:
    command = command.strip()[:80]
    if not command:
        return {"ok": False, "error": "empty command"}
    await manager.send_to_esp32({"type": "device_command", "command": command})
    connection = manager.snapshot()
    return {"ok": True, "command": command, "esp32_connected": connection["esp32_connected"]}


@app.get("/logs", response_class=HTMLResponse)
async def logs_page() -> HTMLResponse:
    return HTMLResponse(_build_logs_page())


@app.get("/logs/audio", response_class=HTMLResponse)
async def audio_logs_page() -> HTMLResponse:
    return HTMLResponse(_build_audio_logs_page())


if app_mount is not None:
    app.mount(config.mcp_mount_path, app_mount)
    logger.info("MCP 外挂接口已挂载到 %s", config.mcp_mount_path)


@app.websocket("/ws")
async def qq_endpoint(websocket: WebSocket) -> None:
    await manager.connect_qq(websocket)
    logger.info(
        "QQ 文本链路已绑定: route=/ws | language=%s | tool=%s",
        config.qq_language_model.model,
        config.qq_tool_model.model,
    )
    try:
        while True:
            payload = json.loads(await websocket.receive_text())
            manager.note_qq_activity()
            await runtime.handle_napcat_payload(payload)
    except WebSocketDisconnect:
        manager.disconnect_qq()
    except Exception as exc:
        logger.exception("QQ 通道异常: %s", exc)
        manager.disconnect_qq()


@app.websocket("/esp32_ws")
async def esp32_endpoint(websocket: WebSocket) -> None:
    await manager.connect_esp32(websocket)
    try:
        while True:
            payload = json.loads(await websocket.receive_text())
            manager.note_esp32_activity(websocket)
            await runtime.handle_esp32_payload(payload)
    except WebSocketDisconnect:
        manager.disconnect_esp32(websocket)
    except Exception as exc:
        logger.exception("ESP32 通道异常: %s", exc)
        manager.disconnect_esp32(websocket)


if __name__ == "__main__":
    import uvicorn

    while True:
        try:
            logger.info("启动并行异构控制脑，端口 8080。")
            logger.info("运行日志: %s", config.runtime_log_file)
            logger.info("模型轨迹/聊天记录日志: %s", config.trace_log_file)
            logger.info(
                "模型路由: qq_text=%s | qq_tool=%s | default_language=%s | default_tool=%s | embedding=%s | vision_low=%s | vision_high=%s | esp32_voice=%s | tts=%s(%s) | tts_voice=%s",
                config.qq_language_model.model,
                config.qq_tool_model.model,
                config.language_model.model,
                config.tool_model.model,
                config.context_embedding.model,
                config.vision_model.model,
                config.vision_highres_model.model,
                (
                    "doubao-realtime-dialog"
                    if runtime.doubao_dialog.available
                    else "doubao-dialog-config-missing"
                    if config.doubao_dialog.enabled
                    else "doubao-dialog-disabled"
                ),
                config.tts_model.model,
                config.tts_model.provider,
                config.tts_voice,
            )
            logger.info("TTS 风格: %s", config.tts_style_prompt)
            logger.info("图片参与回复阈值: %s", config.image_reply_relevance_threshold)
            logger.info(
                "离线时段分析: %s | 阈值 %s 分钟 | 回看 %s 条",
                "开启" if config.offline_gap_analysis_enabled else "关闭",
                config.offline_gap_threshold_minutes,
                config.offline_gap_recent_entries,
            )
            logger.info("外部执行助手 Python 路径: %s", config.generic_agent_python)
            uvicorn.run(app, host="0.0.0.0", port=8080, access_log=False)
            logger.warning("服务退出，5 秒后尝试重新启动。")
        except KeyboardInterrupt:
            logger.info("收到手动停止信号，服务退出。")
            break
        except Exception:
            logger.exception("服务运行失败，5 秒后尝试重新启动。")
        time.sleep(5)
