from __future__ import annotations

import asyncio
import hmac
import re
import json
import logging
import os
import shlex
import socket
import subprocess
import sys
import threading
import time
from collections import deque
from html import escape
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any
import wave

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
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


def _console_field(
    name: str,
    label: str,
    *,
    kind: str = "text",
    note: str = "",
    options: list[str] | None = None,
    value_type: str = "str",
) -> dict[str, Any]:
    return {
        "name": name,
        "label": label,
        "kind": kind,
        "note": note,
        "options": options or [],
        "value_type": value_type,
        "restart_required": True,
    }


_CONSOLE_SECTIONS: list[dict[str, Any]] = [
    {
        "id": "qq",
        "title": "QQ 文本链路",
        "fields": [
            _console_field("QQ_LANGUAGE_API_KEY", "QQ 文本模型 Key", kind="secret"),
            _console_field("QQ_LANGUAGE_BASE_URL", "QQ 文本 Base URL"),
            _console_field("QQ_LANGUAGE_MODEL", "QQ 文本模型"),
            _console_field("QQ_LANGUAGE_TIMEOUT_SECONDS", "QQ 文本超时秒", kind="number", value_type="float"),
            _console_field("QQ_TOOL_MODEL_API_KEY", "QQ 工具模型 Key", kind="secret"),
            _console_field("QQ_TOOL_MODEL_BASE_URL", "QQ 工具 Base URL"),
            _console_field("QQ_TOOL_MODEL", "QQ 工具模型"),
            _console_field("QQ_TOOL_MODEL_TIMEOUT_SECONDS", "QQ 工具超时秒", kind="number", value_type="float"),
            _console_field("QQ_SUMMARY_MAX_CHARS", "QQ 摘要最大字符", kind="number", value_type="int"),
        ],
    },
    {
        "id": "dialog",
        "title": "ESP32 语音和豆包",
        "fields": [
            _console_field("ESP32_DOUBAO_DIALOG_ENABLED", "启用豆包实时语音", kind="bool", value_type="bool"),
            _console_field("DOUBAO_DIALOG_APP_ID", "豆包 App ID"),
            _console_field("DOUBAO_DIALOG_APP_KEY", "豆包 App Key", kind="secret"),
            _console_field("DOUBAO_DIALOG_ACCESS_TOKEN", "豆包 Access Token", kind="secret"),
            _console_field("DOUBAO_DIALOG_RESOURCE_ID", "豆包 Resource ID"),
            _console_field("DOUBAO_DIALOG_WS_URL", "豆包 WebSocket URL"),
            _console_field("DOUBAO_DIALOG_BOT_NAME", "豆包 Bot 名称"),
            _console_field("DOUBAO_DIALOG_SYSTEM_ROLE", "豆包系统角色", kind="textarea"),
            _console_field("DOUBAO_DIALOG_TTS_SPEAKER", "豆包播报音色"),
            _console_field("DOUBAO_DIALOG_TTS_FORMAT", "豆包音频格式"),
            _console_field("DOUBAO_DIALOG_TTS_SAMPLE_RATE", "豆包采样率", kind="number", value_type="int"),
            _console_field("DOUBAO_DIALOG_TTS_CHANNEL", "豆包声道数", kind="number", value_type="int"),
            _console_field("DOUBAO_DIALOG_INPUT_MOD", "豆包输入模式"),
            _console_field("DOUBAO_DIALOG_TIMEOUT_SECONDS", "豆包请求超时秒", kind="number", value_type="float"),
            _console_field("DOUBAO_DIALOG_AUDIO_CHUNK_MS", "豆包音频分片 ms", kind="number", value_type="int"),
            _console_field("DOUBAO_DIALOG_VAD_TAIL_SILENCE_MS", "豆包 VAD 尾静音 ms", kind="number", value_type="int"),
            _console_field("DOUBAO_DIALOG_OUTPUT_FLUSH_MS", "豆包输出刷新 ms", kind="number", value_type="int"),
            _console_field("ESP32_REALTIME_MODE", "ESP32 实时模式", kind="bool", value_type="bool"),
            _console_field("ESP32_REALTIME_SKIP_TEMPORAL_CONTEXT", "实时模式跳过时间上下文", kind="bool", value_type="bool"),
            _console_field("ESP32_REALTIME_SKIP_SEMANTIC_RAG", "实时模式跳过语义检索", kind="bool", value_type="bool"),
            _console_field("ESP32_REALTIME_FAST_CHAT_MAX_CHARS", "快速回复最大字符", kind="number", value_type="int"),
        ],
    },
    {
        "id": "tts",
        "title": "TTS 和下发音频",
        "fields": [
            _console_field("TTS_API_KEY", "TTS Key", kind="secret"),
            _console_field("TTS_BASE_URL", "TTS Base URL"),
            _console_field("TTS_MODEL", "TTS 模型"),
            _console_field("TTS_TIMEOUT_SECONDS", "TTS 超时秒", kind="number", value_type="float"),
            _console_field("MIMO_TTS_API_KEY", "MiMo TTS Key", kind="secret"),
            _console_field("MIMO_TTS_BASE_URL", "MiMo TTS Base URL"),
            _console_field("MIMO_TTS_MODEL", "MiMo TTS 模型"),
            _console_field("MIMO_TTS_PRESET", "MiMo TTS 预设", options=["clear_female", "sweet_female", "soft_female", "bright_female", "calm_male"]),
            _console_field("MIMO_TTS_VOICE", "MiMo TTS 音色"),
            _console_field("MIMO_TTS_STYLE_PROMPT", "MiMo TTS 风格提示", kind="textarea"),
            _console_field("MIMO_TTS_TIMEOUT_SECONDS", "MiMo TTS 超时秒", kind="number", value_type="float"),
            _console_field("ESP32_TTS_PCM_CHUNK_BYTES", "ESP32 TTS PCM 分片字节", kind="number", value_type="int"),
            _console_field("ESP32_TTS_PCM_PACE_RATIO", "ESP32 TTS 播放节奏比例", kind="number", value_type="float"),
            _console_field("ESP32_TTS_PCM_PACE_MIN_MS", "ESP32 TTS 最小间隔 ms", kind="number", value_type="int"),
            _console_field("ESP32_TTS_PCM_PACE_MAX_MS", "ESP32 TTS 最大间隔 ms", kind="number", value_type="int"),
            _console_field("ESP32_POST_TTS_ECHO_GUARD_SECONDS", "TTS 后回声保护秒", kind="number", value_type="float"),
            _console_field("ESP32_POST_TTS_ECHO_MAX_RMS", "TTS 后回声 RMS 上限", kind="number", value_type="float"),
            _console_field("ESP32_POST_TTS_ECHO_MAX_MS", "TTS 后回声时长上限 ms", kind="number", value_type="int"),
            _console_field("MCP_TTS_DEBUG_HISTORY", "保留 TTS 调试历史", kind="bool", value_type="bool"),
            _console_field("MCP_TTS_DEBUG_KEEP", "TTS 调试历史保留数", kind="number", value_type="int"),
        ],
    },
    {
        "id": "models",
        "title": "模型和 API",
        "fields": [
            _console_field("LANGUAGE_API_KEY", "默认语言模型 Key", kind="secret"),
            _console_field("LANGUAGE_BASE_URL", "默认语言 Base URL"),
            _console_field("LANGUAGE_MODEL", "默认语言模型"),
            _console_field("LANGUAGE_TIMEOUT_SECONDS", "默认语言超时秒", kind="number", value_type="float"),
            _console_field("TOOL_MODEL_API_KEY", "默认工具模型 Key", kind="secret"),
            _console_field("TOOL_MODEL_BASE_URL", "默认工具 Base URL"),
            _console_field("TOOL_MODEL", "默认工具模型"),
            _console_field("TOOL_MODEL_TIMEOUT_SECONDS", "默认工具超时秒", kind="number", value_type="float"),
            _console_field("MIMO_API_KEY", "MiMo Key", kind="secret"),
            _console_field("MIMO_BASE_URL", "MiMo Base URL"),
            _console_field("MIMO_LANGUAGE_MODEL", "MiMo 语言模型"),
            _console_field("MIMO_TOOL_MODEL", "MiMo 工具模型"),
            _console_field("MIMO_TIMEOUT_SECONDS", "MiMo 超时秒", kind="number", value_type="float"),
            _console_field("ARK_API_KEY", "火山 Ark Key", kind="secret"),
            _console_field("ARK_BASE_URL", "火山 Ark Base URL"),
            _console_field("ARK_LANGUAGE_MODEL", "火山语言模型"),
            _console_field("ARK_TOOL_MODEL", "火山工具模型"),
            _console_field("DASHSCOPE_API_KEY", "DashScope Key", kind="secret"),
            _console_field("QWEN_API_KEY", "Qwen Key", kind="secret"),
            _console_field("QWEN_BASE_URL", "Qwen Base URL"),
        ],
    },
    {
        "id": "vision",
        "title": "ASR 视觉和检索",
        "fields": [
            _console_field("ASR_API_KEY", "ASR Key", kind="secret"),
            _console_field("ASR_BASE_URL", "ASR Base URL"),
            _console_field("ASR_MODEL", "ASR 模型"),
            _console_field("ASR_TIMEOUT_SECONDS", "ASR 超时秒", kind="number", value_type="float"),
            _console_field("ASR_LANGUAGE", "ASR 语言"),
            _console_field("VISION_LOWRES_API_KEY", "低清视觉 Key", kind="secret"),
            _console_field("VISION_LOWRES_BASE_URL", "低清视觉 Base URL"),
            _console_field("VISION_LOWRES_MODEL", "低清视觉模型"),
            _console_field("VISION_LOWRES_TIMEOUT_SECONDS", "低清视觉超时秒", kind="number", value_type="float"),
            _console_field("VISION_HIGHRES_API_KEY", "高清视觉 Key", kind="secret"),
            _console_field("VISION_HIGHRES_BASE_URL", "高清视觉 Base URL"),
            _console_field("VISION_HIGHRES_MODEL", "高清视觉模型"),
            _console_field("VISION_HIGHRES_TIMEOUT_SECONDS", "高清视觉超时秒", kind="number", value_type="float"),
            _console_field("PROBLEM_MODEL_API_KEY", "题目模型 Key", kind="secret"),
            _console_field("PROBLEM_MODEL_BASE_URL", "题目模型 Base URL"),
            _console_field("PROBLEM_MODEL", "题目模型"),
            _console_field("PROBLEM_MODEL_TIMEOUT_SECONDS", "题目模型超时秒", kind="number", value_type="float"),
            _console_field("CONTEXT_EMBEDDING_API_KEY", "上下文 Embedding Key", kind="secret"),
            _console_field("CONTEXT_EMBEDDING_ENDPOINT", "上下文 Embedding Endpoint"),
            _console_field("CONTEXT_EMBEDDING_MODEL", "上下文 Embedding 模型"),
            _console_field("CONTEXT_EMBEDDING_DIMENSION", "上下文 Embedding 维度", kind="number", value_type="int"),
            _console_field("CONTEXT_EMBEDDING_TIMEOUT_SECONDS", "上下文 Embedding 超时秒", kind="number", value_type="float"),
        ],
    },
    {
        "id": "aiot",
        "title": "日历天气和主动提醒",
        "fields": [
            _console_field("AIOT_WEATHER_LOCATION", "天气位置"),
            _console_field("AIOT_WEATHER_ADCODE", "天气行政区划码"),
            _console_field("AIOT_WEATHER_SCAN_SECONDS", "天气扫描间隔秒", kind="number", value_type="int"),
            _console_field("AIOT_CALENDAR_FILE", "日历文件路径"),
            _console_field("AIOT_PROACTIVE_ENABLED", "启用主动提醒", kind="bool", value_type="bool"),
            _console_field("AIOT_PROACTIVE_TICK_SECONDS", "主动提醒轮询秒", kind="number", value_type="int"),
            _console_field("AIOT_PROACTIVE_MIN_GAP_SECONDS", "主动提醒最小间隔秒", kind="number", value_type="int"),
            _console_field("AIOT_PROACTIVE_VOICE_ENABLED", "主动提醒语音播报", kind="bool", value_type="bool"),
            _console_field("AMAP_API_KEY", "高德 Key", kind="secret"),
            _console_field("SENIVERSE_API_KEY", "心知天气 Key", kind="secret"),
            _console_field("BAILIAN_SEARCH_API_KEY", "百炼搜索 Key", kind="secret"),
            _console_field("BAILIAN_SEARCH_AGENT_ID", "百炼搜索 Agent ID"),
            _console_field("BAILIAN_SEARCH_AGENT_VERSION", "百炼搜索 Agent 版本"),
            _console_field("BAILIAN_WORKSPACE_ID", "百炼 Workspace ID"),
        ],
    },
    {
        "id": "memory",
        "title": "记忆上下文和安全",
        "fields": [
            _console_field("SUBCONSCIOUS_HALF_LIFE_HOURS", "潜意识半衰期小时", kind="number", value_type="float"),
            _console_field("SHARED_CONTEXT_SESSION_ID", "共享上下文 Session"),
            _console_field("CONTEXT_RECENT_TURNS", "最近对话轮数", kind="number", value_type="int"),
            _console_field("CONTEXT_RAG_HITS", "语义检索条数", kind="number", value_type="int"),
            _console_field("MEMORY_HOT_TURNS", "热记忆轮数", kind="number", value_type="int"),
            _console_field("MEMORY_SUMMARY_MIN_TURNS", "记忆摘要最小轮数", kind="number", value_type="int"),
            _console_field("MEMORY_SUMMARY_BATCH_TURNS", "记忆摘要批量轮数", kind="number", value_type="int"),
            _console_field("CONVERSATION_CACHE_SIZE", "会话缓存大小", kind="number", value_type="int"),
            _console_field("IMAGE_REPLY_RELEVANCE_THRESHOLD", "图片追问相关性阈值", kind="number", value_type="int"),
            _console_field("OFFLINE_GAP_ANALYSIS_ENABLED", "启用离线时段分析", kind="bool", value_type="bool"),
            _console_field("OFFLINE_GAP_THRESHOLD_MINUTES", "离线分析阈值分钟", kind="number", value_type="int"),
            _console_field("OFFLINE_GAP_RECENT_ENTRIES", "离线分析最近条数", kind="number", value_type="int"),
            _console_field("HIGH_RISK_APPROVAL_ENABLED", "启用高风险审批", kind="bool", value_type="bool"),
            _console_field("MCP_FACADE_ENABLED", "启用 MCP facade", kind="bool", value_type="bool"),
            _console_field("MCP_MOUNT_PATH", "MCP 挂载路径"),
        ],
    },
    {
        "id": "server",
        "title": "服务端和 NapCat",
        "fields": [
            _console_field("MCP_ROBOT_PORT", "服务端端口", kind="number", value_type="int"),
            _console_field("MCP_ROBOT_DATA_DIR", "数据目录"),
            _console_field("MCP_ROBOT_PROJECT_ROOT", "项目根目录"),
            _console_field("GENERIC_AGENT_ROOT", "GenericAgent 根目录"),
            _console_field("GENERIC_AGENT_PYTHON", "GenericAgent Python"),
            _console_field("MCP_PERSONA_DIR", "角色配置目录"),
            _console_field("MCP_VOICE_PRESET_FILE", "语音预设文件"),
            _console_field("NAPCAT_WEBUI_BASE", "NapCat WebUI Base"),
            _console_field("NAPCAT_WEBUI_TOKEN", "NapCat WebUI Token", kind="secret"),
            _console_field("MCP_CONSOLE_TOKEN", "控制台 Token", kind="secret"),
            _console_field("MCP_CONSOLE_RESTART_COMMAND", "控制台重启命令"),
            _console_field("ESP32_SERVER_MUSIC_DIR", "服务端音乐目录"),
            _console_field("ESP32_SERVER_MUSIC_MAX_SECONDS", "服务端音乐最长秒", kind="number", value_type="float"),
            _console_field("ESP32_SERVER_MUSIC_TARGET_PEAK", "服务端音乐目标峰值", kind="number", value_type="int"),
            _console_field("ESP32_SERVER_MUSIC_MAX_GAIN", "服务端音乐最大增益", kind="number", value_type="float"),
        ],
    },
]

_CONSOLE_FIELD_BY_ENV: dict[str, dict[str, Any]] = {
    field["name"]: field
    for section in _CONSOLE_SECTIONS
    for field in section["fields"]
}
_CONSOLE_FIELD_ORDER = list(_CONSOLE_FIELD_BY_ENV)
_CONSOLE_ENV_LINE_RE = re.compile(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$")
_CONSOLE_SECRET_URL_FIELDS = {
    "QQ_LANGUAGE_API_KEY": "QQ_LANGUAGE_BASE_URL",
    "QQ_TOOL_MODEL_API_KEY": "QQ_TOOL_MODEL_BASE_URL",
    "TTS_API_KEY": "TTS_BASE_URL",
    "MIMO_TTS_API_KEY": "MIMO_TTS_BASE_URL",
    "LANGUAGE_API_KEY": "LANGUAGE_BASE_URL",
    "TOOL_MODEL_API_KEY": "TOOL_MODEL_BASE_URL",
    "MIMO_API_KEY": "MIMO_BASE_URL",
    "ARK_API_KEY": "ARK_BASE_URL",
    "DASHSCOPE_API_KEY": "QWEN_BASE_URL",
    "QWEN_API_KEY": "QWEN_BASE_URL",
    "ASR_API_KEY": "ASR_BASE_URL",
    "VISION_LOWRES_API_KEY": "VISION_LOWRES_BASE_URL",
    "VISION_HIGHRES_API_KEY": "VISION_HIGHRES_BASE_URL",
    "PROBLEM_MODEL_API_KEY": "PROBLEM_MODEL_BASE_URL",
    "CONTEXT_EMBEDDING_API_KEY": "CONTEXT_EMBEDDING_ENDPOINT",
    "BAILIAN_SEARCH_API_KEY": "https://bailian.console.aliyun.com/",
    "AMAP_API_KEY": "https://restapi.amap.com/",
    "SENIVERSE_API_KEY": "https://api.seniverse.com/",
    "DOUBAO_DIALOG_APP_KEY": "DOUBAO_DIALOG_WS_URL",
    "DOUBAO_DIALOG_ACCESS_TOKEN": "DOUBAO_DIALOG_WS_URL",
}


def _console_env_path() -> Path:
    explicit = os.getenv("MCP_ROBOT_ENV_FILE", "").strip()
    if explicit:
        return Path(explicit).expanduser().resolve()
    if os.name == "nt":
        return (config.data_dir / "console.env").resolve()
    return Path("/opt/mcp-robot/secrets.env")


def _parse_console_env_value(raw: str) -> str:
    text = raw.strip()
    if not text:
        return ""
    try:
        parts = shlex.split(text, comments=True, posix=True)
        if parts:
            return parts[0]
    except ValueError:
        pass
    if "#" in text:
        text = text.split("#", 1)[0].rstrip()
    return text.strip().strip("'\"")


def _read_console_env_file(path: Path | None = None) -> dict[str, str]:
    env_path = path or _console_env_path()
    if not env_path.exists():
        return {}
    values: dict[str, str] = {}
    try:
        for line in env_path.read_text(encoding="utf-8").splitlines():
            match = _CONSOLE_ENV_LINE_RE.match(line)
            if match:
                values[match.group(1)] = _parse_console_env_value(match.group(2))
    except OSError as exc:
        logger.warning("控制台读取环境文件失败: %s", exc)
    return values


def _quote_console_env_value(value: str) -> str:
    normalized = re.sub(r"\s*\r?\n\s*", " ", str(value or "")).strip()
    escaped = (
        normalized.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("$", "\\$")
        .replace("`", "\\`")
    )
    return f'"{escaped}"'


def _console_secret_hint(value: str) -> str:
    if not value:
        return ""
    if len(value) <= 8:
        return "已设置"
    return f"已设置: ...{value[-4:]}"


def _console_display_value(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    if value is None:
        return ""
    return str(value)


def _console_runtime_defaults() -> dict[str, str]:
    active_runtime = globals().get("runtime")
    server_music_dir = getattr(active_runtime, "server_music_dir", "")
    return {
        "QQ_LANGUAGE_BASE_URL": _console_display_value(config.qq_language_model.base_url),
        "QQ_LANGUAGE_MODEL": _console_display_value(config.qq_language_model.model),
        "QQ_LANGUAGE_TIMEOUT_SECONDS": _console_display_value(config.qq_language_model.timeout_seconds),
        "QQ_TOOL_MODEL_BASE_URL": _console_display_value(config.qq_tool_model.base_url),
        "QQ_TOOL_MODEL": _console_display_value(config.qq_tool_model.model),
        "QQ_TOOL_MODEL_TIMEOUT_SECONDS": _console_display_value(config.qq_tool_model.timeout_seconds),
        "QQ_SUMMARY_MAX_CHARS": _console_display_value(config.qq_summary_max_chars),
        "ESP32_DOUBAO_DIALOG_ENABLED": _console_display_value(config.doubao_dialog.enabled),
        "DOUBAO_DIALOG_APP_ID": _console_display_value(config.doubao_dialog.app_id),
        "DOUBAO_DIALOG_RESOURCE_ID": _console_display_value(config.doubao_dialog.resource_id),
        "DOUBAO_DIALOG_WS_URL": _console_display_value(config.doubao_dialog.ws_url),
        "DOUBAO_DIALOG_BOT_NAME": _console_display_value(config.doubao_dialog.bot_name),
        "DOUBAO_DIALOG_SYSTEM_ROLE": _console_display_value(config.doubao_dialog.system_role),
        "DOUBAO_DIALOG_TTS_SPEAKER": _console_display_value(config.doubao_dialog.tts_speaker),
        "DOUBAO_DIALOG_TTS_FORMAT": _console_display_value(config.doubao_dialog.tts_format),
        "DOUBAO_DIALOG_TTS_SAMPLE_RATE": _console_display_value(config.doubao_dialog.tts_sample_rate),
        "DOUBAO_DIALOG_TTS_CHANNEL": _console_display_value(config.doubao_dialog.tts_channel),
        "DOUBAO_DIALOG_INPUT_MOD": _console_display_value(config.doubao_dialog.input_mod),
        "DOUBAO_DIALOG_TIMEOUT_SECONDS": _console_display_value(config.doubao_dialog.timeout_seconds),
        "DOUBAO_DIALOG_AUDIO_CHUNK_MS": _console_display_value(config.doubao_dialog.audio_chunk_ms),
        "DOUBAO_DIALOG_VAD_TAIL_SILENCE_MS": _console_display_value(config.doubao_dialog.vad_tail_silence_ms),
        "DOUBAO_DIALOG_OUTPUT_FLUSH_MS": _console_display_value(config.doubao_dialog.output_flush_ms),
        "ESP32_REALTIME_MODE": _console_display_value(config.esp32_realtime_mode),
        "ESP32_REALTIME_SKIP_TEMPORAL_CONTEXT": _console_display_value(config.esp32_realtime_skip_temporal_context),
        "ESP32_REALTIME_SKIP_SEMANTIC_RAG": _console_display_value(config.esp32_realtime_skip_semantic_rag),
        "ESP32_REALTIME_FAST_CHAT_MAX_CHARS": _console_display_value(config.esp32_realtime_fast_chat_max_chars),
        "TTS_BASE_URL": _console_display_value(config.tts_model.base_url),
        "TTS_MODEL": _console_display_value(config.tts_model.model),
        "TTS_TIMEOUT_SECONDS": _console_display_value(config.tts_model.timeout_seconds),
        "MIMO_TTS_BASE_URL": os.getenv("MIMO_TTS_BASE_URL", os.getenv("MIMO_BASE_URL", config.tts_model.base_url)),
        "MIMO_TTS_MODEL": os.getenv("MIMO_TTS_MODEL", config.tts_model.model),
        "MIMO_TTS_PRESET": os.getenv("MIMO_TTS_PRESET", "clear_female"),
        "MIMO_TTS_VOICE": _console_display_value(config.tts_voice),
        "MIMO_TTS_STYLE_PROMPT": _console_display_value(config.tts_style_prompt),
        "MIMO_TTS_TIMEOUT_SECONDS": os.getenv("MIMO_TTS_TIMEOUT_SECONDS", _console_display_value(config.tts_model.timeout_seconds)),
        "ESP32_TTS_PCM_CHUNK_BYTES": os.getenv("ESP32_TTS_PCM_CHUNK_BYTES", "3200"),
        "ESP32_TTS_PCM_PACE_RATIO": os.getenv("ESP32_TTS_PCM_PACE_RATIO", "0.90"),
        "ESP32_TTS_PCM_PACE_MIN_MS": os.getenv("ESP32_TTS_PCM_PACE_MIN_MS", "20"),
        "ESP32_TTS_PCM_PACE_MAX_MS": os.getenv("ESP32_TTS_PCM_PACE_MAX_MS", "420"),
        "ESP32_POST_TTS_ECHO_GUARD_SECONDS": os.getenv("ESP32_POST_TTS_ECHO_GUARD_SECONDS", "2.5"),
        "ESP32_POST_TTS_ECHO_MAX_RMS": os.getenv("ESP32_POST_TTS_ECHO_MAX_RMS", "700"),
        "ESP32_POST_TTS_ECHO_MAX_MS": os.getenv("ESP32_POST_TTS_ECHO_MAX_MS", "1500"),
        "MCP_TTS_DEBUG_HISTORY": os.getenv("MCP_TTS_DEBUG_HISTORY", "0"),
        "MCP_TTS_DEBUG_KEEP": os.getenv("MCP_TTS_DEBUG_KEEP", "20"),
        "LANGUAGE_BASE_URL": _console_display_value(config.language_model.base_url),
        "LANGUAGE_MODEL": _console_display_value(config.language_model.model),
        "LANGUAGE_TIMEOUT_SECONDS": _console_display_value(config.language_model.timeout_seconds),
        "TOOL_MODEL_BASE_URL": _console_display_value(config.tool_model.base_url),
        "TOOL_MODEL": _console_display_value(config.tool_model.model),
        "TOOL_MODEL_TIMEOUT_SECONDS": _console_display_value(config.tool_model.timeout_seconds),
        "MIMO_BASE_URL": os.getenv("MIMO_BASE_URL", "https://api.xiaomimimo.com/v1"),
        "MIMO_LANGUAGE_MODEL": os.getenv("MIMO_LANGUAGE_MODEL", config.language_model.model),
        "MIMO_TOOL_MODEL": os.getenv("MIMO_TOOL_MODEL", config.tool_model.model),
        "MIMO_TIMEOUT_SECONDS": os.getenv("MIMO_TIMEOUT_SECONDS", "120"),
        "ARK_BASE_URL": os.getenv("ARK_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3"),
        "ARK_LANGUAGE_MODEL": os.getenv("ARK_LANGUAGE_MODEL", ""),
        "ARK_TOOL_MODEL": os.getenv("ARK_TOOL_MODEL", ""),
        "QWEN_BASE_URL": os.getenv("QWEN_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1"),
        "ASR_BASE_URL": _console_display_value(config.asr_model.base_url),
        "ASR_MODEL": _console_display_value(config.asr_model.model),
        "ASR_TIMEOUT_SECONDS": _console_display_value(config.asr_model.timeout_seconds),
        "ASR_LANGUAGE": _console_display_value(config.asr_language),
        "VISION_LOWRES_BASE_URL": _console_display_value(config.vision_model.base_url),
        "VISION_LOWRES_MODEL": _console_display_value(config.vision_model.model),
        "VISION_LOWRES_TIMEOUT_SECONDS": _console_display_value(config.vision_model.timeout_seconds),
        "VISION_HIGHRES_BASE_URL": _console_display_value(config.vision_highres_model.base_url),
        "VISION_HIGHRES_MODEL": _console_display_value(config.vision_highres_model.model),
        "VISION_HIGHRES_TIMEOUT_SECONDS": _console_display_value(config.vision_highres_model.timeout_seconds),
        "PROBLEM_MODEL_BASE_URL": _console_display_value(config.problem_model.base_url),
        "PROBLEM_MODEL": _console_display_value(config.problem_model.model),
        "PROBLEM_MODEL_TIMEOUT_SECONDS": _console_display_value(config.problem_model.timeout_seconds),
        "CONTEXT_EMBEDDING_ENDPOINT": _console_display_value(config.context_embedding.endpoint),
        "CONTEXT_EMBEDDING_MODEL": _console_display_value(config.context_embedding.model),
        "CONTEXT_EMBEDDING_DIMENSION": _console_display_value(config.context_embedding.dimension),
        "CONTEXT_EMBEDDING_TIMEOUT_SECONDS": _console_display_value(config.context_embedding.timeout_seconds),
        "AIOT_WEATHER_LOCATION": _console_display_value(config.aiot_weather_location),
        "AIOT_WEATHER_ADCODE": _console_display_value(config.aiot_weather_adcode),
        "AIOT_WEATHER_SCAN_SECONDS": _console_display_value(config.aiot_weather_scan_seconds),
        "AIOT_CALENDAR_FILE": _console_display_value(config.aiot_calendar_file),
        "AIOT_PROACTIVE_ENABLED": _console_display_value(config.proactive_enabled),
        "AIOT_PROACTIVE_TICK_SECONDS": _console_display_value(config.proactive_tick_seconds),
        "AIOT_PROACTIVE_MIN_GAP_SECONDS": _console_display_value(config.proactive_min_gap_seconds),
        "AIOT_PROACTIVE_VOICE_ENABLED": _console_display_value(config.proactive_voice_enabled),
        "BAILIAN_SEARCH_AGENT_ID": _console_display_value(config.tool_api.quark_search_agent_id),
        "BAILIAN_SEARCH_AGENT_VERSION": _console_display_value(config.tool_api.quark_search_agent_version),
        "BAILIAN_WORKSPACE_ID": _console_display_value(config.tool_api.quark_search_workspace_id),
        "SUBCONSCIOUS_HALF_LIFE_HOURS": _console_display_value(config.subconscious_half_life_hours),
        "SHARED_CONTEXT_SESSION_ID": _console_display_value(config.shared_session_id),
        "CONTEXT_RECENT_TURNS": _console_display_value(config.context_recent_turns),
        "CONTEXT_RAG_HITS": _console_display_value(config.context_rag_hits),
        "MEMORY_HOT_TURNS": _console_display_value(config.memory_hot_turns),
        "MEMORY_SUMMARY_MIN_TURNS": _console_display_value(config.memory_summary_min_turns),
        "MEMORY_SUMMARY_BATCH_TURNS": _console_display_value(config.memory_summary_batch_turns),
        "CONVERSATION_CACHE_SIZE": _console_display_value(config.conversation_cache_size),
        "IMAGE_REPLY_RELEVANCE_THRESHOLD": _console_display_value(config.image_reply_relevance_threshold),
        "OFFLINE_GAP_ANALYSIS_ENABLED": _console_display_value(config.offline_gap_analysis_enabled),
        "OFFLINE_GAP_THRESHOLD_MINUTES": _console_display_value(config.offline_gap_threshold_minutes),
        "OFFLINE_GAP_RECENT_ENTRIES": _console_display_value(config.offline_gap_recent_entries),
        "HIGH_RISK_APPROVAL_ENABLED": _console_display_value(config.high_risk_approval_enabled),
        "MCP_FACADE_ENABLED": _console_display_value(config.mcp_facade_enabled),
        "MCP_MOUNT_PATH": _console_display_value(config.mcp_mount_path),
        "MCP_ROBOT_PORT": os.getenv("MCP_ROBOT_PORT", "8080"),
        "MCP_ROBOT_DATA_DIR": _console_display_value(config.data_dir),
        "MCP_ROBOT_PROJECT_ROOT": _console_display_value(config.project_root),
        "GENERIC_AGENT_ROOT": _console_display_value(config.generic_agent_root),
        "GENERIC_AGENT_PYTHON": _console_display_value(config.generic_agent_python),
        "MCP_PERSONA_DIR": _console_display_value(config.doubao_dialog.persona_dir),
        "MCP_VOICE_PRESET_FILE": _console_display_value(config.doubao_dialog.voice_preset_file),
        "NAPCAT_WEBUI_BASE": os.getenv("NAPCAT_WEBUI_BASE", "http://127.0.0.1:6099"),
        "MCP_CONSOLE_RESTART_COMMAND": os.getenv("MCP_CONSOLE_RESTART_COMMAND", "systemctl restart mcp-robot.service"),
        "ESP32_SERVER_MUSIC_DIR": _console_display_value(server_music_dir),
        "ESP32_SERVER_MUSIC_MAX_SECONDS": os.getenv("ESP32_SERVER_MUSIC_MAX_SECONDS", "300"),
        "ESP32_SERVER_MUSIC_TARGET_PEAK": os.getenv("ESP32_SERVER_MUSIC_TARGET_PEAK", "16000"),
        "ESP32_SERVER_MUSIC_MAX_GAIN": os.getenv("ESP32_SERVER_MUSIC_MAX_GAIN", "4.0"),
    }


def _console_current_value(name: str, env_file_values: dict[str, str], runtime_values: dict[str, str]) -> tuple[str, str]:
    if name in env_file_values:
        return env_file_values[name], "env_file"
    if name in os.environ:
        return os.getenv(name, ""), "process"
    if name in runtime_values:
        return runtime_values[name], "runtime"
    return "", "unset"


def _console_secret_reference_url(name: str, env_file_values: dict[str, str], runtime_values: dict[str, str]) -> str:
    ref = _CONSOLE_SECRET_URL_FIELDS.get(name, "")
    if not ref:
        return ""
    if re.match(r"^https?://", ref, flags=re.IGNORECASE):
        return ref
    value, _source = _console_current_value(ref, env_file_values, runtime_values)
    return value


def _console_allowed_tokens() -> list[tuple[str, str]]:
    env_file_values = _read_console_env_file()
    tokens: list[tuple[str, str]] = []
    for name in ("MCP_CONSOLE_TOKEN", "NAPCAT_WEBUI_TOKEN"):
        for source, value in (("process", os.getenv(name, "").strip()), ("env_file", env_file_values.get(name, "").strip())):
            if value and all(not hmac.compare_digest(value, existing) for existing, _ in tokens):
                tokens.append((value, f"{name}:{source}"))
    return tokens


def _console_expected_token() -> tuple[str, str]:
    tokens = _console_allowed_tokens()
    if tokens:
        return tokens[0]
    return "", ""


def _console_auth_sources() -> list[str]:
    return [source for _, source in _console_allowed_tokens()]


def _console_has_matching_token(provided: str) -> bool:
    if not provided:
        return False
    for value, _source in _console_allowed_tokens():
        if value:
            try:
                if hmac.compare_digest(provided, value):
                    return True
            except TypeError:
                continue
    return False


def _console_request_token(request: Request) -> str:
    query_token = str(request.query_params.get("token") or "").strip()
    if query_token:
        return query_token
    header_token = str(request.headers.get("x-console-token") or "").strip()
    if header_token:
        return header_token
    auth = str(request.headers.get("authorization") or "").strip()
    if auth.lower().startswith("bearer "):
        return auth[7:].strip()
    return ""


def _require_console_auth(request: Request) -> None:
    sources = _console_auth_sources()
    if not sources:
        raise HTTPException(status_code=503, detail="console token is not configured")
    provided = _console_request_token(request)
    if not _console_has_matching_token(provided):
        raise HTTPException(status_code=401, detail="invalid console token")


def _normalize_console_bool(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    text = str(value or "").strip().lower()
    return "1" if text in {"1", "true", "yes", "on", "y", "是", "开", "开启"} else "0"


def _normalize_console_field_value(field: dict[str, Any], value: Any) -> str:
    kind = field.get("kind", "text")
    value_type = field.get("value_type", "str")
    if kind == "bool" or value_type == "bool":
        return _normalize_console_bool(value)
    text = str(value if value is not None else "").strip()
    if not text:
        return ""
    if value_type == "int":
        return str(int(float(text)))
    if value_type == "float":
        return str(float(text))
    if len(text) > 4096:
        raise ValueError(f"{field['name']} is too long")
    return text


def _write_console_env_file(updates: dict[str, str], clear_keys: set[str]) -> dict[str, Any]:
    env_path = _console_env_path()
    env_path.parent.mkdir(parents=True, exist_ok=True)
    existing_lines = env_path.read_text(encoding="utf-8").splitlines() if env_path.exists() else []
    remaining_updates = dict(updates)
    clear_keys = {key for key in clear_keys if key in _CONSOLE_FIELD_BY_ENV}
    changed_keys: list[str] = []
    output_lines: list[str] = []
    handled_updates: set[str] = set()

    for line in existing_lines:
        match = _CONSOLE_ENV_LINE_RE.match(line)
        if not match:
            output_lines.append(line.rstrip())
            continue
        key = match.group(1)
        if key in clear_keys:
            changed_keys.append(key)
            remaining_updates.pop(key, None)
            continue
        if key in handled_updates:
            continue
        if key in remaining_updates:
            output_lines.append(f"{key}={_quote_console_env_value(remaining_updates.pop(key, ''))}")
            handled_updates.add(key)
            changed_keys.append(key)
            continue
        output_lines.append(line.rstrip())

    new_keys = [key for key in _CONSOLE_FIELD_ORDER if key in remaining_updates]
    if new_keys:
        if output_lines and output_lines[-1].strip():
            output_lines.append("")
        output_lines.append("# Managed by MCP robot console")
        for key in new_keys:
            output_lines.append(f"{key}={_quote_console_env_value(remaining_updates[key])}")
            changed_keys.append(key)

    text = "\n".join(output_lines).rstrip() + "\n"
    tmp_path = env_path.with_name(f".{env_path.name}.tmp")
    tmp_path.write_text(text, encoding="utf-8")
    if os.name != "nt":
        try:
            os.chmod(tmp_path, 0o600)
        except OSError:
            pass
    os.replace(tmp_path, env_path)
    return {"env_file": str(env_path), "changed": changed_keys}


def _console_config_payload() -> dict[str, Any]:
    env_path = _console_env_path()
    env_file_values = _read_console_env_file(env_path)
    runtime_values = _console_runtime_defaults()
    sections: list[dict[str, Any]] = []
    for section in _CONSOLE_SECTIONS:
        fields: list[dict[str, Any]] = []
        for field in section["fields"]:
            name = field["name"]
            value, source = _console_current_value(name, env_file_values, runtime_values)
            item = {key: value_ for key, value_ in field.items() if key != "value"}
            item["source"] = source
            if field.get("kind") == "secret":
                item["value"] = ""
                item["secret_set"] = bool(value)
                item["secret_hint"] = _console_secret_hint(value)
                item["reference_url"] = _console_secret_reference_url(name, env_file_values, runtime_values)
            else:
                item["value"] = value
            fields.append(item)
        sections.append({"id": section["id"], "title": section["title"], "fields": fields})
    return {
        "ok": True,
        "env_file": str(env_path),
        "env_file_exists": env_path.exists(),
        "token_configured": bool(_console_auth_sources()),
        "token_source": ", ".join(_console_auth_sources()),
        "restart_required": True,
        "sections": sections,
        "read_only": [
            {
                "title": "闹钟音乐",
                "value": "当前云端事件默认 alarm_music_index=2；固件的 reminder_alarm 仍按本地默认第三首 TF 卡音乐触发。",
            },
            {
                "title": "闹钟倒计时",
                "value": "倒计时和日历事件由云端日历文件保存；实际屏幕呈现由 ESP32 固件消费事件快照。",
            },
        ],
    }


def _schedule_console_restart() -> None:
    command = os.getenv("MCP_CONSOLE_RESTART_COMMAND", "systemctl restart mcp-robot.service").strip()
    if not command:
        raise RuntimeError("restart command is empty")

    def _worker() -> None:
        time.sleep(0.35)
        try:
            subprocess.run(
                shlex.split(command),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=30,
                check=False,
            )
        except Exception as exc:
            logger.exception("控制台触发服务重启失败: %s", exc)

    threading.Thread(target=_worker, name="console-service-restart", daemon=True).start()


def _build_console_page() -> str:
    return """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MCP-Robot 控制台</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f6f7f9;
      --panel: #ffffff;
      --panel-soft: #f9fafb;
      --line: #d9dee7;
      --text: #172033;
      --muted: #667085;
      --accent: #0f766e;
      --accent-soft: #d9f1ee;
      --warn: #a15c07;
      --danger: #b42318;
      --ok: #047857;
      font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", system-ui, sans-serif;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
    }
    .wrap {
      width: min(1360px, calc(100vw - 32px));
      margin: 0 auto;
      padding: 24px 0 40px;
    }
    header.top {
      display: flex;
      justify-content: space-between;
      gap: 18px;
      align-items: flex-start;
      padding-bottom: 16px;
      border-bottom: 1px solid var(--line);
    }
    h1 {
      margin: 0;
      font-size: 26px;
      line-height: 1.2;
      letter-spacing: 0;
    }
    .sub {
      color: var(--muted);
      font-size: 13px;
      margin-top: 7px;
      word-break: break-all;
    }
    .nav {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
      justify-content: flex-end;
    }
    .nav a, button {
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--text);
      border-radius: 8px;
      padding: 9px 12px;
      font-size: 13px;
      font-weight: 650;
      text-decoration: none;
      cursor: pointer;
      min-height: 38px;
    }
    .nav a.active, button.primary {
      background: var(--accent);
      border-color: var(--accent);
      color: #fff;
    }
    button.danger {
      border-color: #f2b8b5;
      color: var(--danger);
    }
    button:disabled {
      opacity: .55;
      cursor: not-allowed;
    }
    .login {
      margin-top: 18px;
      display: grid;
      grid-template-columns: minmax(220px, 420px) auto;
      gap: 10px;
      align-items: end;
    }
    .bar {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 10px;
      margin-top: 16px;
      padding: 12px;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }
    .pill {
      display: inline-flex;
      align-items: center;
      min-height: 28px;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 4px 8px;
      background: var(--panel-soft);
      color: var(--muted);
      font-size: 12px;
      max-width: 100%;
      word-break: break-all;
    }
    .pill.ok {
      border-color: #b7e3cf;
      background: #e9f8f0;
      color: var(--ok);
    }
    .pill.warn {
      border-color: #f1d6a8;
      background: #fff6e6;
      color: var(--warn);
    }
    .layout {
      display: grid;
      grid-template-columns: 240px minmax(0, 1fr);
      gap: 16px;
      margin-top: 16px;
    }
    .tabs {
      display: grid;
      gap: 6px;
      align-content: start;
      position: sticky;
      top: 12px;
    }
    .tab {
      text-align: left;
      background: transparent;
      border-color: transparent;
      color: var(--muted);
    }
    .tab.active {
      background: var(--accent-soft);
      border-color: #a6d8d2;
      color: #0f524c;
    }
    .section {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      overflow: hidden;
      margin-bottom: 14px;
    }
    .section h2 {
      margin: 0;
      padding: 14px 16px;
      font-size: 16px;
      border-bottom: 1px solid var(--line);
      letter-spacing: 0;
    }
    .fields {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 0;
    }
    .field {
      min-width: 0;
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
    }
    .field:nth-child(odd) {
      border-right: 1px solid var(--line);
    }
    .label-row {
      display: flex;
      gap: 8px;
      justify-content: space-between;
      align-items: flex-start;
      min-height: 24px;
      margin-bottom: 8px;
    }
    label {
      font-size: 13px;
      font-weight: 700;
      line-height: 1.35;
    }
    .source {
      flex: 0 0 auto;
      color: var(--muted);
      font-size: 11px;
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 2px 6px;
      background: var(--panel-soft);
    }
    input, select, textarea {
      width: 100%;
      border: 1px solid var(--line);
      background: #fff;
      color: var(--text);
      border-radius: 8px;
      padding: 9px 10px;
      font-size: 13px;
      line-height: 1.35;
      outline: none;
    }
    textarea {
      min-height: 92px;
      resize: vertical;
    }
    .secret-row {
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      gap: 8px;
      align-items: center;
    }
    .secret-row button {
      min-width: 66px;
    }
    input:focus, select:focus, textarea:focus {
      border-color: #55aaa2;
      box-shadow: 0 0 0 3px rgba(15, 118, 110, .12);
    }
    .bool-row {
      display: flex;
      align-items: center;
      gap: 9px;
      min-height: 38px;
    }
    .bool-row input {
      width: 18px;
      height: 18px;
      accent-color: var(--accent);
    }
    .clear-row {
      display: flex;
      gap: 8px;
      align-items: center;
      margin-top: 8px;
      color: var(--muted);
      font-size: 12px;
    }
    .clear-row input {
      width: 16px;
      height: 16px;
      accent-color: var(--danger);
    }
    .note {
      margin-top: 6px;
      color: var(--muted);
      font-size: 12px;
      line-height: 1.45;
      word-break: break-all;
    }
    .mono {
      font-family: Consolas, "SFMono-Regular", monospace;
    }
    .readonly {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
      margin-top: 14px;
    }
    .readonly-item {
      background: #fffdf7;
      border: 1px solid #ead8b4;
      border-radius: 8px;
      padding: 12px 14px;
    }
    .readonly-item b {
      display: block;
      font-size: 13px;
      margin-bottom: 6px;
    }
    .readonly-item span {
      color: #6d4b16;
      font-size: 13px;
      line-height: 1.5;
    }
    .status {
      min-height: 20px;
      color: var(--muted);
      font-size: 13px;
      margin-left: auto;
    }
    .status.ok { color: var(--ok); }
    .status.warn { color: var(--warn); }
    .status.err { color: var(--danger); }
    .hidden {
      display: none !important;
    }
    @media (max-width: 980px) {
      header.top, .layout, .fields, .readonly, .login {
        grid-template-columns: 1fr;
      }
      header.top {
        display: grid;
      }
      .nav {
        justify-content: flex-start;
      }
      .tabs {
        position: static;
        grid-template-columns: repeat(2, minmax(0, 1fr));
      }
      .field:nth-child(odd) {
        border-right: 0;
      }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <header class="top">
      <div>
        <h1>MCP-Robot 控制台</h1>
        <div class="sub" id="env-file">环境文件: 未连接</div>
      </div>
      <nav class="nav">
        <a class="active" href="/console">控制台</a>
        <a href="/logs">日志</a>
        <a href="/logs/audio">音频</a>
        <a href="/music">音乐</a>
      </nav>
    </header>

    <section class="login" id="login">
      <input id="token-input" type="password" autocomplete="current-password" placeholder="控制台 Token">
      <button class="primary" id="connect">连接</button>
    </section>

    <section class="bar hidden" id="toolbar">
      <button class="primary" id="save" disabled>保存</button>
      <button id="reload">刷新</button>
      <button class="danger" id="restart">重启服务</button>
      <span class="pill warn">保存后重启生效</span>
      <span class="pill" id="health-pill">健康状态未加载</span>
      <span class="status" id="status"></span>
    </section>

    <section class="readonly hidden" id="readonly"></section>

    <main class="layout hidden" id="main">
      <aside class="tabs" id="tabs"></aside>
      <div id="sections"></div>
    </main>
  </div>

  <script>
    const state = {
      token: '',
      config: null,
      fields: new Map(),
      dirty: new Set(),
      clear: new Set(),
      active: '',
    };

    function qs(selector) { return document.querySelector(selector); }
    function qsa(selector) { return Array.from(document.querySelectorAll(selector)); }
    function esc(value) {
      return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
    }
    function setStatus(text, kind = '') {
      const node = qs('#status');
      node.textContent = text || '';
      node.className = `status ${kind}`.trim();
    }
    function authHeaders(json = false) {
      const headers = { 'X-Console-Token': state.token };
      if (json) headers['Content-Type'] = 'application/json';
      return headers;
    }
    async function api(path, options = {}) {
      const response = await fetch(path, {
        cache: 'no-store',
        ...options,
        headers: { ...(options.headers || {}), ...authHeaders(Boolean(options.body)) },
      });
      const text = await response.text();
      let payload = {};
      try { payload = text ? JSON.parse(text) : {}; } catch (_) { payload = { detail: text }; }
      if (!response.ok || payload.ok === false) {
        throw new Error(payload.detail || payload.error || response.statusText);
      }
      return payload;
    }
    function initToken() {
      const params = new URLSearchParams(location.search);
      const fromUrl = params.get('token') || '';
      if (fromUrl) {
        state.token = fromUrl;
        sessionStorage.setItem('mcp_console_token', fromUrl);
        history.replaceState(null, '', location.pathname);
      } else {
        state.token = sessionStorage.getItem('mcp_console_token') || '';
      }
      qs('#token-input').value = state.token;
    }
    function fieldInputId(name) {
      return `field-${name.replaceAll('_', '-')}`;
    }
    function sourceLabel(source) {
      if (source === 'env_file') return '文件';
      if (source === 'process') return '进程';
      return '未设置';
    }
    function renderInput(field) {
      const id = fieldInputId(field.name);
      if (field.kind === 'bool') {
        const checked = ['1', 'true', 'yes', 'on'].includes(String(field.value || '').toLowerCase());
        return `<div class="bool-row"><input id="${id}" data-name="${esc(field.name)}" type="checkbox" ${checked ? 'checked' : ''}><span>${checked ? '开启' : '关闭'}</span></div>`;
      }
      if (field.kind === 'textarea') {
        return `<textarea id="${id}" data-name="${esc(field.name)}" spellcheck="false">${esc(field.value || '')}</textarea>`;
      }
      if (field.options && field.options.length) {
        const current = String(field.value || '');
        const opts = [''].concat(field.options).map((item) => `<option value="${esc(item)}" ${item === current ? 'selected' : ''}>${esc(item || '未指定')}</option>`).join('');
        return `<select id="${id}" data-name="${esc(field.name)}">${opts}</select>`;
      }
      if (field.kind === 'secret') {
        const hint = field.secret_hint || (field.secret_set ? '已设置' : '未设置');
        return `
          <div class="secret-row">
            <input id="${id}" data-name="${esc(field.name)}" type="password" autocomplete="new-password" placeholder="${esc(hint)}">
            <button type="button" data-reveal="${esc(field.name)}">${field.secret_set ? '显示' : '无值'}</button>
          </div>
          ${field.reference_url ? `<div class="note">URL: <span class="mono">${esc(field.reference_url)}</span></div>` : ''}
          <div class="note hidden" data-secret-view="${esc(field.name)}"></div>
          <label class="clear-row"><input data-clear="${esc(field.name)}" type="checkbox">清空当前值</label>
        `;
      }
      const type = field.kind === 'number' ? 'number' : 'text';
      const step = field.value_type === 'int' ? '1' : 'any';
      return `<input id="${id}" data-name="${esc(field.name)}" type="${type}" step="${step}" value="${esc(field.value || '')}" spellcheck="false">`;
    }
    function renderField(field) {
      return `
        <div class="field" data-field="${esc(field.name)}">
          <div class="label-row">
            <label for="${fieldInputId(field.name)}">${esc(field.label)}</label>
            <span class="source">${sourceLabel(field.source)}</span>
          </div>
          ${renderInput(field)}
          ${field.note ? `<div class="note">${esc(field.note)}</div>` : ''}
        </div>
      `;
    }
    function bindFieldEvents() {
      qsa('[data-name]').forEach((node) => {
        const name = node.dataset.name;
        const handler = () => {
          state.dirty.add(name);
          qs('#save').disabled = false;
          if (node.type === 'checkbox') {
            const text = node.parentElement.querySelector('span');
            if (text) text.textContent = node.checked ? '开启' : '关闭';
          }
        };
        node.addEventListener('input', handler);
        node.addEventListener('change', handler);
      });
      qsa('[data-clear]').forEach((node) => {
        node.addEventListener('change', () => {
          if (node.checked) state.clear.add(node.dataset.clear);
          else state.clear.delete(node.dataset.clear);
          qs('#save').disabled = false;
        });
      });
      qsa('[data-reveal]').forEach((button) => {
        button.addEventListener('click', async () => {
          const name = button.dataset.reveal;
          const view = document.querySelector(`[data-secret-view="${CSS.escape(name)}"]`);
          if (!view || button.textContent === '无值') return;
          if (!view.classList.contains('hidden')) {
            view.textContent = '';
            view.classList.add('hidden');
            button.textContent = '显示';
            return;
          }
          button.disabled = true;
          button.textContent = '读取';
          try {
            const data = await api(`/api/console/secret/${encodeURIComponent(name)}`);
            if (data.value) {
              view.textContent = `密钥: ${data.value}`;
              view.classList.remove('hidden');
              button.textContent = '隐藏';
            } else {
              view.textContent = '密钥未设置';
              view.classList.remove('hidden');
              button.textContent = '无值';
            }
          } catch (err) {
            setStatus(err.message || String(err), 'err');
            button.textContent = '显示';
          } finally {
            button.disabled = false;
          }
        });
      });
    }
    function renderConfig(data) {
      state.config = data;
      state.fields = new Map();
      state.dirty.clear();
      state.clear.clear();
      qs('#save').disabled = true;
      qs('#login').classList.add('hidden');
      qs('#toolbar').classList.remove('hidden');
      qs('#readonly').classList.remove('hidden');
      qs('#main').classList.remove('hidden');
      qs('#env-file').textContent = `环境文件: ${data.env_file || ''}`;
      qs('#health-pill').textContent = data.token_configured ? `Token: ${data.token_source}` : 'Token 未配置';
      qs('#health-pill').className = data.token_configured ? 'pill ok' : 'pill warn';

      const sections = data.sections || [];
      if (!state.active && sections.length) state.active = sections[0].id;
      qs('#tabs').innerHTML = sections.map((section) => `
        <button class="tab ${section.id === state.active ? 'active' : ''}" data-tab="${esc(section.id)}">${esc(section.title)}</button>
      `).join('');
      qs('#sections').innerHTML = sections.map((section) => {
        section.fields.forEach((field) => state.fields.set(field.name, field));
        return `
          <section class="section ${section.id === state.active ? '' : 'hidden'}" data-section="${esc(section.id)}">
            <h2>${esc(section.title)}</h2>
            <div class="fields">${section.fields.map(renderField).join('')}</div>
          </section>
        `;
      }).join('');
      qs('#readonly').innerHTML = (data.read_only || []).map((item) => `
        <div class="readonly-item"><b>${esc(item.title)}</b><span>${esc(item.value)}</span></div>
      `).join('');
      qsa('[data-tab]').forEach((button) => {
        button.addEventListener('click', () => {
          state.active = button.dataset.tab;
          qsa('[data-tab]').forEach((item) => item.classList.toggle('active', item.dataset.tab === state.active));
          qsa('[data-section]').forEach((item) => item.classList.toggle('hidden', item.dataset.section !== state.active));
        });
      });
      bindFieldEvents();
    }
    async function loadConfig() {
      if (!state.token) {
        qs('#login').classList.remove('hidden');
        setStatus('需要 Token', 'warn');
        return;
      }
      setStatus('加载中...');
      const data = await api('/api/console/config');
      renderConfig(data);
      setStatus('已加载', 'ok');
    }
    function collectValues() {
      const values = {};
      for (const name of state.dirty) {
        if (state.clear.has(name)) continue;
        const field = state.fields.get(name);
        const node = qs(`#${CSS.escape(fieldInputId(name))}`);
        if (!field || !node) continue;
        if (field.kind === 'secret' && !node.value) continue;
        if (field.kind === 'bool') values[name] = node.checked ? '1' : '0';
        else values[name] = node.value;
      }
      return { values, clear: Array.from(state.clear) };
    }
    async function saveConfig() {
      const payload = collectValues();
      if (!Object.keys(payload.values).length && !payload.clear.length) {
        setStatus('没有待保存的修改', 'warn');
        return;
      }
      qs('#save').disabled = true;
      setStatus('保存中...');
      const result = await api('/api/console/config', { method: 'POST', body: JSON.stringify(payload) });
      setStatus(`已保存 ${result.changed_count || 0} 项`, 'ok');
      await loadConfig();
    }
    async function restartService() {
      setStatus('正在发送重启命令...');
      await api('/api/console/restart', { method: 'POST', body: JSON.stringify({}) });
      setStatus('重启命令已发送，稍后刷新', 'ok');
      setTimeout(() => loadConfig().catch((err) => setStatus(`等待服务恢复: ${err.message}`, 'warn')), 3500);
    }
    qs('#connect').addEventListener('click', () => {
      state.token = qs('#token-input').value.trim();
      if (state.token) sessionStorage.setItem('mcp_console_token', state.token);
      loadConfig().catch((err) => {
        qs('#login').classList.remove('hidden');
        setStatus(err.message, 'err');
      });
    });
    qs('#token-input').addEventListener('keydown', (event) => {
      if (event.key === 'Enter') qs('#connect').click();
    });
    qs('#reload').addEventListener('click', () => loadConfig().catch((err) => setStatus(err.message, 'err')));
    qs('#save').addEventListener('click', () => saveConfig().catch((err) => {
      qs('#save').disabled = false;
      setStatus(err.message, 'err');
    }));
    qs('#restart').addEventListener('click', () => restartService().catch((err) => setStatus(err.message, 'err')));
    initToken();
    loadConfig().catch((err) => {
      qs('#login').classList.remove('hidden');
      setStatus(err.message, 'err');
    });
  </script>
</body>
</html>"""


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
        <a href="/music">服务端音乐</a>
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
        <a href="/music">服务端音乐</a>
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


def _build_server_music_page() -> str:
    return """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>服务端音乐</title>
  <style>
    :root {
      --bg: #f7f8fb;
      --panel: #ffffff;
      --line: #dde3ee;
      --text: #172033;
      --muted: #667085;
      --accent: #2563eb;
      --accent-soft: #dbeafe;
      --danger: #b91c1c;
      --ok: #047857;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font-family: Inter, "Microsoft YaHei", system-ui, sans-serif;
    }
    .wrap {
      width: min(1120px, calc(100vw - 32px));
      margin: 0 auto;
      padding: 28px 0 40px;
    }
    .top {
      display: flex;
      align-items: flex-start;
      justify-content: space-between;
      gap: 18px;
      margin-bottom: 18px;
    }
    h1 {
      margin: 0;
      font-size: 28px;
      line-height: 1.2;
      letter-spacing: 0;
    }
    .sub {
      margin-top: 8px;
      color: var(--muted);
      font-size: 14px;
      word-break: break-all;
    }
    .nav {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
      justify-content: flex-end;
    }
    .nav a, button {
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--text);
      border-radius: 8px;
      padding: 9px 12px;
      font: inherit;
      cursor: pointer;
      text-decoration: none;
      min-height: 38px;
    }
    button.primary {
      background: var(--accent);
      border-color: var(--accent);
      color: #fff;
    }
    button.danger {
      background: #fee2e2;
      border-color: #fecaca;
      color: var(--danger);
    }
    button:disabled {
      opacity: .45;
      cursor: not-allowed;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 16px;
      margin-bottom: 14px;
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 12px;
    }
    .stat {
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 12px;
      min-height: 78px;
    }
    .stat b {
      display: block;
      font-size: 13px;
      color: var(--muted);
      margin-bottom: 8px;
    }
    .stat span {
      font-size: 15px;
      word-break: break-all;
    }
    .toolbar {
      display: grid;
      grid-template-columns: 1fr 140px auto auto auto auto;
      gap: 10px;
      align-items: center;
    }
    input {
      width: 100%;
      border: 1px solid var(--line);
      background: #fff;
      color: var(--text);
      border-radius: 8px;
      padding: 10px 12px;
      font: inherit;
      min-height: 40px;
    }
    .list {
      display: grid;
      gap: 8px;
    }
    .row {
      display: grid;
      grid-template-columns: 52px 1fr 110px 92px;
      gap: 10px;
      align-items: center;
      border: 1px solid var(--line);
      background: #fff;
      border-radius: 8px;
      padding: 10px;
    }
    .row.active {
      border-color: var(--accent);
      background: var(--accent-soft);
    }
    .idx {
      color: var(--muted);
      font-size: 13px;
    }
    .name {
      font-weight: 650;
      word-break: break-all;
    }
    .meta {
      color: var(--muted);
      font-size: 13px;
      text-align: right;
    }
    .empty, .error {
      color: var(--muted);
      padding: 14px;
    }
    .error {
      color: var(--danger);
    }
    .ok {
      color: var(--ok);
    }
    @media (max-width: 860px) {
      .top, .nav { display: block; }
      .nav { margin-top: 14px; }
      .status-grid, .toolbar, .row {
        grid-template-columns: 1fr;
      }
      .meta { text-align: left; }
    }
  </style>
</head>
<body>
  <main class="wrap">
    <section class="top">
      <div>
        <h1>服务端音乐</h1>
        <div class="sub" id="music-dir">加载中</div>
      </div>
      <div class="nav">
        <a href="/logs">日志</a>
        <a href="/logs/audio">音频波形</a>
        <a class="active" href="/music">服务端音乐</a>
      </div>
    </section>
    <section class="panel status-grid">
      <div class="stat"><b>ESP32</b><span id="esp32-status">加载中</span></div>
      <div class="stat"><b>播放状态</b><span id="play-status">加载中</span></div>
      <div class="stat"><b>当前曲目</b><span id="current-track">无</span></div>
      <div class="stat"><b>连续对话</b><span id="chat-status">加载中</span></div>
    </section>
    <section class="panel">
      <div class="toolbar">
        <input id="search" type="text" placeholder="搜索歌名">
        <input id="max-seconds" type="number" min="1" max="600" step="1" value="60" title="最大播放秒数">
        <button class="primary" id="play-selected">播放选中</button>
        <button id="prev">上一首</button>
        <button id="next">下一首</button>
        <button class="danger" id="stop">停止</button>
      </div>
      <div class="sub">最大播放秒数用于测试和演示，正式播放可以改到 300 秒或 600 秒。</div>
    </section>
    <section class="panel">
      <div class="list" id="music-list"><div class="empty">加载中</div></div>
    </section>
  </main>
  <script>
    let files = [];
    let status = {};
    let selectedIndex = -1;
    let busy = false;
    function escapeHtml(value) {
      return String(value ?? '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
    }
    function fmtBytes(bytes) {
      const value = Number(bytes || 0);
      if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MB`;
      if (value >= 1024) return `${(value / 1024).toFixed(1)} KB`;
      return `${value} B`;
    }
    function maxSeconds() {
      const raw = Number(document.getElementById('max-seconds').value || 60);
      return Math.max(1, Math.min(600, raw));
    }
    function setBusy(value) {
      busy = value;
      for (const id of ['play-selected', 'prev', 'next', 'stop']) {
        document.getElementById(id).disabled = value;
      }
    }
    function filteredFiles() {
      const q = document.getElementById('search').value.trim().toLowerCase();
      if (!q) return files;
      return files.filter((item) => String(item.name || '').toLowerCase().includes(q));
    }
    function render() {
      document.getElementById('music-dir').textContent = status.dir || '';
      const connection = status.connection || {};
      const music = status.status || {};
      const espAge = connection.esp32_last_activity_age_s == null ? '无' : `${Number(connection.esp32_last_activity_age_s).toFixed(1)}s`;
      document.getElementById('esp32-status').innerHTML = connection.esp32_connected
        ? `<span class="ok">在线</span> · ${escapeHtml(espAge)}`
        : '<span class="error">离线</span>';
      document.getElementById('play-status').textContent = music.active ? '播放中' : '空闲';
      document.getElementById('current-track').textContent = music.current_file || '无';
      const chat = music.continuous_chat;
      document.getElementById('chat-status').textContent =
        `${chat === true ? '开启' : chat === false ? '关闭' : '未知'}${music.restore_continuous_pending ? ' · 播完恢复' : ''}`;
      const root = document.getElementById('music-list');
      const shown = filteredFiles();
      if (!shown.length) {
        root.innerHTML = '<div class="empty">没有匹配的音乐文件。</div>';
        return;
      }
      root.innerHTML = shown.map((item) => {
        const active = item.index === music.current_index || item.index === selectedIndex;
        return `
          <div class="row ${active ? 'active' : ''}" data-index="${item.index}">
            <div class="idx">#${String(item.index + 1).padStart(2, '0')}</div>
            <div class="name">${escapeHtml(item.name)}</div>
            <div class="meta">${escapeHtml(item.suffix || '')} · ${fmtBytes(item.bytes)}</div>
            <button data-play="${item.index}">播放</button>
          </div>
        `;
      }).join('');
      for (const row of root.querySelectorAll('.row')) {
        row.addEventListener('click', (event) => {
          if (event.target && event.target.dataset && event.target.dataset.play) return;
          selectedIndex = Number(row.dataset.index);
          render();
        });
      }
      for (const button of root.querySelectorAll('button[data-play]')) {
        button.addEventListener('click', () => playIndex(Number(button.dataset.play)));
      }
    }
    async function loadAll() {
      const [fileResp, statusResp] = await Promise.all([
        fetch('/api/esp32/server-music-files?limit=200', { cache: 'no-store' }),
        fetch('/api/esp32/server-music-status', { cache: 'no-store' }),
      ]);
      const fileData = await fileResp.json();
      const statusData = await statusResp.json();
      files = fileData.files || [];
      status = statusData || {};
      if (selectedIndex < 0 && files.length) {
        selectedIndex = status.status?.selected_index ?? files[0].index;
      }
      render();
    }
    async function playIndex(index) {
      setBusy(true);
      try {
        const params = new URLSearchParams({ index: String(index), max_seconds: String(maxSeconds()) });
        const response = await fetch(`/api/esp32/server-music-play?${params.toString()}`, { cache: 'no-store' });
        if (!response.ok) throw new Error((await response.json()).detail || response.statusText);
        selectedIndex = index;
        await loadAll();
      } catch (err) {
        alert(`播放失败: ${err.message || err}`);
      } finally {
        setBusy(false);
      }
    }
    async function command(name) {
      setBusy(true);
      try {
        const params = new URLSearchParams({ command: name, max_seconds: String(maxSeconds()) });
        const response = await fetch(`/api/esp32/server-music-command?${params.toString()}`, { cache: 'no-store' });
        if (!response.ok) throw new Error((await response.json()).detail || response.statusText);
        await loadAll();
      } catch (err) {
        alert(`命令失败: ${err.message || err}`);
      } finally {
        setBusy(false);
      }
    }
    document.getElementById('search').addEventListener('input', render);
    document.getElementById('play-selected').addEventListener('click', () => {
      if (selectedIndex >= 0) playIndex(selectedIndex);
    });
    document.getElementById('prev').addEventListener('click', () => command('prev'));
    document.getElementById('next').addEventListener('click', () => command('next'));
    document.getElementById('stop').addEventListener('click', () => command('stop'));
    loadAll();
    setInterval(loadAll, 3000);
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

    def note_qq_activity(self, websocket: WebSocket | None = None) -> None:
        if websocket is not None and self.qq_client is not websocket:
            self.qq_client = websocket
            if self.qq_connected_at <= 0:
                self.qq_connected_at = time.time()
            logger.info("QQ 终端连接已从上行活动恢复。")
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
        self.note_qq_activity(websocket)
        logger.info("QQ 终端 (NapCat) 已连接。")

    async def connect_esp32(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self.esp32_client = websocket
        self.esp32_connected_at = time.time()
        self._esp32_tts_chunk_count = 0
        self._esp32_tts_b64_chars = 0
        self.note_esp32_activity()
        logger.info("ESP32 终端已连接。")

    def disconnect_qq(self, websocket: WebSocket | None = None) -> None:
        if websocket is not None and self.qq_client is not websocket:
            logger.debug("忽略旧 QQ socket 的断开事件。")
            return
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
            self.note_qq_activity(self.qq_client)
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
            self.disconnect_qq(self.qq_client)
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
                if msg_type == "tts_pcm_chunk" and (
                    self._esp32_tts_chunk_count <= 3 or self._esp32_tts_chunk_count % 10 == 0
                ):
                    logger.info(
                        "ESP32 下行 TTS PCM 分片: chunk_no=%s pcm_bytes=%s b64_chars=%d",
                        message.get("chunk_no"),
                        message.get("pcm_bytes", ""),
                        len(audio_b64),
                    )
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


@app.get("/api/console/config")
async def api_console_config(request: Request) -> dict[str, Any]:
    _require_console_auth(request)
    return _console_config_payload()


@app.get("/api/console/secret/{name}")
async def api_console_secret(request: Request, name: str) -> dict[str, Any]:
    _require_console_auth(request)
    field_name = str(name or "").strip()
    field = _CONSOLE_FIELD_BY_ENV.get(field_name)
    if field is None or field.get("kind") != "secret":
        raise HTTPException(status_code=404, detail="secret field not found")
    env_file_values = _read_console_env_file()
    runtime_values = _console_runtime_defaults()
    value, source = _console_current_value(field_name, env_file_values, runtime_values)
    return {
        "ok": True,
        "name": field_name,
        "value": value,
        "source": source,
        "reference_url": _console_secret_reference_url(field_name, env_file_values, runtime_values),
    }


@app.post("/api/console/config")
async def api_console_config_save(request: Request, payload: dict[str, Any]) -> dict[str, Any]:
    _require_console_auth(request)
    raw_values = payload.get("values") or {}
    if not isinstance(raw_values, dict):
        raise HTTPException(status_code=400, detail="values must be an object")
    raw_clear = payload.get("clear") or []
    if not isinstance(raw_clear, list):
        raise HTTPException(status_code=400, detail="clear must be a list")

    updates: dict[str, str] = {}
    clear_keys: set[str] = set()
    for key in raw_clear:
        name = str(key or "").strip()
        if name not in _CONSOLE_FIELD_BY_ENV:
            raise HTTPException(status_code=400, detail=f"unsupported field: {name}")
        clear_keys.add(name)

    for raw_name, raw_value in raw_values.items():
        name = str(raw_name or "").strip()
        field = _CONSOLE_FIELD_BY_ENV.get(name)
        if field is None:
            raise HTTPException(status_code=400, detail=f"unsupported field: {name}")
        if name in clear_keys:
            continue
        if field.get("kind") == "secret" and str(raw_value or "") == "":
            continue
        try:
            normalized = _normalize_console_field_value(field, raw_value)
        except (TypeError, ValueError) as exc:
            raise HTTPException(status_code=400, detail=f"{name}: {exc}") from exc
        if normalized == "":
            clear_keys.add(name)
        else:
            updates[name] = normalized

    if not updates and not clear_keys:
        return {"ok": True, "changed_count": 0, "changed": [], "env_file": str(_console_env_path())}

    try:
        result = _write_console_env_file(updates, clear_keys)
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"env file write failed: {exc}") from exc

    return {
        "ok": True,
        "changed_count": len(result["changed"]),
        "changed": result["changed"],
        "env_file": result["env_file"],
        "restart_required": True,
    }


@app.post("/api/console/restart")
async def api_console_restart(request: Request) -> dict[str, Any]:
    _require_console_auth(request)
    if os.name == "nt" and not os.getenv("MCP_CONSOLE_RESTART_COMMAND", "").strip():
        return {"ok": False, "detail": "restart command is not configured on Windows"}
    try:
        _schedule_console_restart()
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return {"ok": True, "scheduled": True}


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


def _esp32_wav_downlink_dirs() -> dict[str, Path]:
    return {
        "server": (config.data_dir / "server_wav").resolve(),
        "tts": (config.data_dir / "esp32_tts").resolve(),
        "recording": (config.data_dir / "esp32_audio").resolve(),
    }


def _select_esp32_downlink_wav(source: str = "server", file: str = "", min_bytes: int = 44) -> tuple[str, Path]:
    dirs = _esp32_wav_downlink_dirs()
    source_key = re.sub(r"[^a-z_]", "", str(source or "").strip().lower()) or "server"
    if source_key not in dirs:
        raise HTTPException(status_code=400, detail=f"Unsupported WAV source: {source_key}")

    wav_dir = dirs[source_key]
    if source_key == "server":
        wav_dir.mkdir(parents=True, exist_ok=True)

    wav_path: Path | None = None
    if file.strip():
        candidate = (wav_dir / Path(file).name).resolve()
        if candidate.parent != wav_dir or not candidate.is_file() or candidate.suffix.lower() != ".wav":
            raise HTTPException(status_code=404, detail="WAV file not found")
        wav_path = candidate
    else:
        wavs = sorted(
            (path for path in wav_dir.glob("*.wav") if path.is_file() and path.stat().st_size >= max(0, min_bytes)),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        if wavs:
            wav_path = wavs[0]
    if wav_path is None:
        raise HTTPException(status_code=404, detail=f"No WAV file available in {source_key}")
    return source_key, wav_path


@app.get("/api/esp32/wav-downlink-files")
async def api_esp32_wav_downlink_files(source: str = "server", limit: int = 20) -> dict[str, Any]:
    dirs = _esp32_wav_downlink_dirs()
    source_key = re.sub(r"[^a-z_]", "", str(source or "").strip().lower()) or "server"
    if source_key not in dirs:
        raise HTTPException(status_code=400, detail=f"Unsupported WAV source: {source_key}")
    wav_dir = dirs[source_key]
    if source_key == "server":
        wav_dir.mkdir(parents=True, exist_ok=True)
    files = []
    for path in sorted(wav_dir.glob("*.wav"), key=lambda item: item.stat().st_mtime, reverse=True)[: max(1, min(limit, 100))]:
        try:
            files.append(
                {
                    "name": path.name,
                    "bytes": path.stat().st_size,
                    "mtime": path.stat().st_mtime,
                }
            )
        except OSError:
            continue
    return {"ok": True, "source": source_key, "dir": str(wav_dir), "files": files}


@app.get("/api/esp32/wav-downlink")
async def api_esp32_wav_downlink(
    source: str = "server",
    file: str = "",
    max_seconds: float = 60.0,
) -> dict[str, Any]:
    source_key, wav_path = _select_esp32_downlink_wav(source=source, file=file)
    try:
        result = await runtime.send_esp32_wav_downlink(
            wav_path,
            label=f"{source_key}/{wav_path.name}",
            max_seconds=max(0.5, min(float(max_seconds or 60.0), 300.0)),
        )
    except RuntimeError as exc:
        detail = str(exc)
        status_code = 409 if "not connected" in detail.lower() else 400
        raise HTTPException(status_code=status_code, detail=detail) from exc
    return {"ok": True, "source": source_key, **result}


@app.get("/api/esp32/server-music-files")
async def api_esp32_server_music_files(limit: int = 50) -> dict[str, Any]:
    return {
        "ok": True,
        "dir": str(runtime.server_music_dir),
        "files": runtime.list_esp32_server_music_files(limit=max(1, min(limit, 200))),
    }


@app.get("/api/esp32/server-music-status")
async def api_esp32_server_music_status() -> dict[str, Any]:
    return {
        "ok": True,
        "connection": manager.snapshot(),
        "status": runtime.get_esp32_server_music_status(),
        "dir": str(runtime.server_music_dir),
    }


@app.get("/api/esp32/server-music-play")
async def api_esp32_server_music_play(
    file: str = "",
    index: int | None = None,
    max_seconds: float = 300.0,
    wait: bool = False,
) -> dict[str, Any]:
    try:
        if wait:
            selected_index, path = runtime._select_server_music_path(file=file, index=index)
            result = await runtime.send_esp32_audio_file_downlink(
                path,
                label=f"server_music/{path.name}",
                max_seconds=max(0.5, min(float(max_seconds or 300.0), 600.0)),
            )
            return {"ok": True, "action": "play_wait", "index": selected_index, **result}
        return await runtime.control_esp32_server_music(
            "music_play",
            file=file,
            index=index,
            max_seconds=max(0.5, min(float(max_seconds or 300.0), 600.0)),
        )
    except RuntimeError as exc:
        detail = str(exc)
        status_code = 409 if "not connected" in detail.lower() else 400
        raise HTTPException(status_code=status_code, detail=detail) from exc


@app.get("/api/esp32/server-music-command")
async def api_esp32_server_music_command(command: str = "music_play", max_seconds: float = 300.0) -> dict[str, Any]:
    normalized = command.strip().lower()
    aliases = {
        "play": "music_play",
        "stop": "music_stop",
        "pause": "music_stop",
        "toggle": "music_toggle",
        "next": "music_next",
        "prev": "music_prev",
        "previous": "music_prev",
    }
    normalized = aliases.get(normalized, normalized)
    if normalized not in {"music_play", "music_stop", "music_toggle", "music_next", "music_prev"}:
        raise HTTPException(status_code=400, detail="Unsupported music command")
    try:
        return await runtime.control_esp32_server_music(
            normalized,
            max_seconds=max(0.5, min(float(max_seconds or 300.0), 600.0)),
        )
    except RuntimeError as exc:
        detail = str(exc)
        status_code = 409 if "not connected" in detail.lower() else 400
        raise HTTPException(status_code=status_code, detail=detail) from exc


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


@app.get("/console", response_class=HTMLResponse)
async def console_page() -> HTMLResponse:
    return HTMLResponse(_build_console_page())


@app.get("/logs/audio", response_class=HTMLResponse)
async def audio_logs_page() -> HTMLResponse:
    return HTMLResponse(_build_audio_logs_page())


@app.get("/music", response_class=HTMLResponse)
async def server_music_page() -> HTMLResponse:
    return HTMLResponse(_build_server_music_page())


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
            manager.note_qq_activity(websocket)
            await runtime.handle_napcat_payload(payload)
    except WebSocketDisconnect:
        manager.disconnect_qq(websocket)
    except Exception as exc:
        logger.exception("QQ 通道异常: %s", exc)
        manager.disconnect_qq(websocket)


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

    app_port = int(os.getenv("MCP_ROBOT_PORT", "8080"))
    while True:
        try:
            logger.info("启动并行异构控制脑，端口 %s。", app_port)
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
            uvicorn.run(app, host="0.0.0.0", port=app_port, access_log=False)
            logger.warning("服务退出，5 秒后尝试重新启动。")
        except KeyboardInterrupt:
            logger.info("收到手动停止信号，服务退出。")
            break
        except Exception:
            logger.exception("服务运行失败，5 秒后尝试重新启动。")
        time.sleep(5)
