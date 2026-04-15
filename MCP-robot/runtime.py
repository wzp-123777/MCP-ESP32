from __future__ import annotations

import asyncio
import json
import logging
from dataclasses import dataclass, field
from typing import Any, Protocol

from app_config import AppConfig
from frame_store import FrameStore
from generic_agent_bridge import GenericAgentBridge
from memory_store import SubconsciousMemoryStore
from models import LanguageModelService, SemanticPlan, ToolModelService, TTSModelService, VisionAnalysis, VisionModelService
from tools import AmapTool, HighResVisionTool, SeniverseWeatherTool, TavilySearchTool, ToolExecutionBundle, ToolRegistry


logger = logging.getLogger("MCP_Robot.Runtime")
trace_logger = logging.getLogger("MCP_Robot.Trace")


class ConnectionManagerProtocol(Protocol):
    async def send_to_qq(self, message: dict[str, Any]) -> None: ...

    async def send_to_esp32(self, message: dict[str, Any]) -> None: ...


@dataclass(slots=True)
class TextRequest:
    source: str
    text: str
    user_id: str = ""
    device_id: str = ""
    message_type: str = "private"
    group_id: str | None = None
    extra: dict[str, Any] = field(default_factory=dict)


class StreamingTTSDispatcher:
    def __init__(self, tts_service: TTSModelService, emit_chunk) -> None:
        self.tts_service = tts_service
        self.emit_chunk = emit_chunk
        self._queue: asyncio.Queue[tuple[int, str] | None] = asyncio.Queue()
        self._buffer = ""
        self._segment_no = 0
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
                if char in "。！？!?；;\n" and index >= 6:
                    cut_index = index
                    break
            if cut_index >= 0:
                segment = self._buffer[: cut_index + 1].strip()
                self._buffer = self._buffer[cut_index + 1 :]
                if segment:
                    segments.append(segment)
                continue
            if not final and len(self._buffer) >= 48:
                segment = self._buffer[:48].strip()
                self._buffer = self._buffer[48:]
                if segment:
                    segments.append(segment)
                continue
            break
        if final and self._buffer.strip():
            segments.append(self._buffer.strip())
            self._buffer = ""
        return segments

    async def _run(self) -> None:
        while True:
            item = await self._queue.get()
            if item is None:
                await self.emit_chunk({"type": "tts_stream_end"})
                return
            segment_no, segment_text = item
            await self.emit_chunk({"type": "tts_segment_start", "segment_no": segment_no, "text": segment_text})
            try:
                async for audio in self.tts_service.stream_audio(segment_text):
                    payload = {
                        "type": "tts_audio_chunk",
                        "segment_no": segment_no,
                        "text": segment_text,
                        **audio,
                    }
                    await self.emit_chunk(payload)
            except Exception as exc:
                logger.warning("TTS streaming failed: %s", exc)
                await self.emit_chunk(
                    {
                        "type": "tts_error",
                        "segment_no": segment_no,
                        "text": segment_text,
                        "error": str(exc),
                    }
                )


class RobotRuntime:
    def __init__(self, config: AppConfig, connection_manager: ConnectionManagerProtocol) -> None:
        self.config = config
        self.connection_manager = connection_manager
        self.language_model = LanguageModelService(config.language_model)
        self.tool_model = ToolModelService(config.tool_model)
        self.vision_model = VisionModelService(config.vision_model)
        self.vision_highres_model = VisionModelService(config.vision_highres_model)
        self.tts_model = TTSModelService(config.tts_model, config.tts_voice) if config.tts_model.api_key else None
        self.memory_store = SubconsciousMemoryStore(
            config.subconscious_file,
            half_life_hours=config.subconscious_half_life_hours,
        )
        self.frame_store = FrameStore(config.data_dir / "frames")
        self.agent_bridge = GenericAgentBridge(config.generic_agent_root, config.generic_agent_python)
        self.tool_registry = self._build_tool_registry()
        self._tasks: set[asyncio.Task[Any]] = set()

    def _build_tool_registry(self) -> ToolRegistry:
        registry = ToolRegistry()
        registry.register(SeniverseWeatherTool(self.config.tool_api.seniverse_key))
        registry.register(TavilySearchTool(self.config.tool_api.tavily_key))
        registry.register(AmapTool(self.config.tool_api.amap_key))
        registry.register(HighResVisionTool(self.frame_store, self.vision_highres_model))
        return registry

    def _trace(self, event: str, **payload: Any) -> None:
        body = {"event": event, **payload}
        try:
            trace_logger.info(json.dumps(body, ensure_ascii=False, default=str))
        except Exception:
            trace_logger.info("%s | %s", event, payload)

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
            "tavily",
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
        )
        return not any(marker in stripped for marker in tool_markers)

    def _detect_fast_tool_route(self, text: str) -> SemanticPlan | None:
        stripped = text.strip()
        lowered = stripped.lower()
        if not stripped or len(stripped) > 120 or "\n" in stripped or lowered.startswith("root/"):
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
            "tavily",
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

    def _track_task(self, coro) -> None:
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
        task.add_done_callback(self._log_background_task)

    def _log_background_task(self, task: asyncio.Task[Any]) -> None:
        try:
            task.result()
        except asyncio.CancelledError:
            pass
        except Exception:
            logger.exception("Background task failed")

    async def handle_napcat_payload(self, payload: dict[str, Any]) -> None:
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
            extra=payload,
        )
        self._trace(
            "chat.incoming",
            source=request.source,
            user_id=request.user_id,
            message_type=request.message_type,
            group_id=request.group_id,
            text=request.text,
        )
        if raw_message.startswith("root/"):
            self._track_task(self._run_root_command(request))
        else:
            self._track_task(self._run_text_conversation(request))

    async def handle_esp32_payload(self, payload: dict[str, Any]) -> None:
        event_type = str(payload.get("type") or "unknown")
        device_id = str(payload.get("device_id") or "ESP32_Core_1")
        if event_type == "audio_text":
            content = str(payload.get("content") or "").strip()
            if not content:
                return
            request = TextRequest(source="ESP32", text=content, device_id=device_id, extra=payload)
            self._trace("chat.incoming", source=request.source, device_id=device_id, text=content)
            self._track_task(self._run_text_conversation(request))
        elif event_type == "image":
            image_base64 = str(payload.get("image_base64") or payload.get("data") or "").strip()
            if not image_base64:
                logger.warning("ESP32 image payload missing image_base64/data field")
                return
            self._trace(
                "vision.frame.incoming",
                source="ESP32",
                device_id=device_id,
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                caption=str(payload.get("caption") or ""),
            )
            self._track_task(self._run_image_pipeline(device_id, payload))
        elif event_type == "telemetry":
            logger.info("ESP32 telemetry from %s: %s", device_id, json.dumps(payload, ensure_ascii=False))
        else:
            logger.info("Unhandled ESP32 event %s from %s", event_type, device_id)

    async def _run_root_command(self, request: TextRequest) -> None:
        try:
            command = request.text[len("root/") :].strip()
            if not command:
                await self._send_qq_reply(request, "root/ 后面没有命令。")
                return
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
            logger.exception("root command failed")
            self._trace("root.command.error", command=request.text, error=str(exc))
            await self._send_qq_reply(request, f"root agent 执行失败: {exc}")

    async def _run_image_pipeline(self, device_id: str, payload: dict[str, Any]) -> None:
        try:
            frame = self.frame_store.save_frame(
                device_id=device_id,
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                image_base64=str(payload.get("image_base64") or payload.get("data") or ""),
                note=str(payload.get("caption") or ""),
            )
            analysis = await self.vision_model.observe_environment(
                image_base64=str(payload.get("image_base64") or payload.get("data") or ""),
                mime_type=str(payload.get("mime_type") or "image/jpeg"),
                source_hint="esp32-camera",
                caption=str(payload.get("caption") or ""),
            )
            self.frame_store.update_summary(frame.frame_id, analysis.scene_summary)
            self._trace(
                "vision.observe",
                frame_id=frame.frame_id,
                device_id=device_id,
                model=self.vision_model.model_name,
                summary=analysis.scene_summary,
                should_reply=analysis.should_reply,
                confidence=analysis.confidence,
                importance=analysis.importance,
            )
            if analysis.device_action:
                await self.connection_manager.send_to_esp32({"type": "device_command", "command": analysis.device_action})
            if analysis.should_reply:
                vision_text = analysis.suggested_reply or "请根据摄像头画面做出一句合理回应。"
                request = TextRequest(
                    source="ESP32",
                    text=vision_text,
                    device_id=device_id,
                    extra={"vision_analysis": analysis.scene_summary},
                )
                await self._run_text_conversation(request, vision_analysis=analysis)
                return
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
        except Exception as exc:
            logger.exception("Image pipeline failed")
            self._trace("vision.observe.error", device_id=device_id, error=str(exc))
            await self.connection_manager.send_to_esp32(
                {
                    "type": "vision_error",
                    "device_id": device_id,
                    "error": str(exc),
                }
            )

    async def _run_text_conversation(
        self,
        request: TextRequest,
        *,
        vision_analysis: VisionAnalysis | None = None,
    ) -> None:
        try:
            subconscious_context = self.memory_store.build_context()
            latest_frame_context = self.frame_store.summary_context()
            if latest_frame_context:
                subconscious_context = f"{latest_frame_context}\n{subconscious_context}".strip()
            if vision_analysis is None:
                semantic_plan: SemanticPlan | None = None
                fast_route = ""
                semantic_plan = self._detect_fast_tool_route(request.text)
                if semantic_plan is not None:
                    fast_route = f"tool:{semantic_plan.intent}"
                elif self._is_fast_chat_candidate(request.text):
                    semantic_plan = SemanticPlan(
                        intent="chat",
                        needs_tools=False,
                        should_reply=True,
                        tool_goal=request.text,
                        response_style="像即时聊天一样简短自然，直接回答。",
                    )
                    fast_route = "chat"

                if semantic_plan is not None:
                    self._trace(
                        "semantic.plan.fast_route",
                        source=request.source,
                        user_text=request.text,
                        model="fast_path",
                        route=fast_route,
                        intent=semantic_plan.intent,
                        needs_tools=semantic_plan.needs_tools,
                    )
                else:
                    semantic_plan = await self.language_model.plan_text(
                        source=request.source,
                        user_text=request.text,
                        subconscious=subconscious_context,
                    )
            else:
                semantic_plan = await self.language_model.plan_text(
                    source=request.source,
                    user_text=request.text,
                    subconscious=subconscious_context,
                )
            self._trace(
                "semantic.plan",
                source=request.source,
                user_text=request.text,
                model=self.language_model.model_name,
                intent=semantic_plan.intent,
                needs_tools=semantic_plan.needs_tools,
                should_reply=semantic_plan.should_reply,
                tool_goal=semantic_plan.tool_goal,
            )
            if not semantic_plan.should_reply:
                self.memory_store.remember(
                    source=request.source.lower(),
                    summary=f"未回复消息保留上下文: {request.text}",
                    confidence=0.35,
                    importance=0.2,
                )
                return

            tool_bundle = ToolExecutionBundle()
            if semantic_plan.needs_tools:
                tool_plan = await self.tool_model.plan_tools(
                    user_text=request.text,
                    semantic_plan=semantic_plan,
                    tool_catalog=self.tool_registry.catalog(),
                    extra_context=latest_frame_context,
                )
                self._trace(
                    "tool.plan",
                    source=request.source,
                    user_text=request.text,
                    model=self.tool_model.model_name,
                    calls=tool_plan.calls,
                    return_to_language=tool_plan.return_to_language,
                    direct_reply=tool_plan.direct_reply,
                    device_commands=tool_plan.device_commands,
                )
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

            esp32_ready = bool(getattr(self.connection_manager, "is_esp32_connected", True))
            tts_dispatcher = (
                StreamingTTSDispatcher(self.tts_model, self.connection_manager.send_to_esp32)
                if self.tts_model and esp32_ready
                else None
            )
            reply_chunks: list[str] = []
            tool_context = tool_bundle.to_prompt_block()
            vision_context = ""
            if vision_analysis is not None:
                vision_context = f"视觉观察: {vision_analysis.scene_summary}\n建议方向: {vision_analysis.suggested_reply}"

            async for chunk in self.language_model.stream_reply(
                source=request.source,
                user_text=request.text,
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

            final_reply = "".join(reply_chunks).strip()
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

            if request.source == "NapCatQQ":
                qq_summary = ""
                if self._should_build_qq_summary(final_reply):
                    qq_summary = await self.language_model.summarize_for_qq(
                        user_text=request.text,
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
        except Exception as exc:
            logger.exception("Conversation pipeline failed")
            self._trace("chat.error", source=request.source, text=request.text, error=str(exc))
            if request.source == "NapCatQQ":
                await self._send_qq_reply(request, f"本轮处理失败: {exc}")
            else:
                await self.connection_manager.send_to_esp32(
                    {
                        "type": "assistant_error",
                        "device_id": request.device_id,
                        "error": str(exc),
                    }
                )

    async def _dispatch_direct_reply(self, request: TextRequest, text: str) -> None:
        self._trace("chat.direct_reply", source=request.source, text=text)
        if request.source == "NapCatQQ":
            await self._send_qq_reply(request, text)
        else:
            await self.connection_manager.send_to_esp32(
                {
                    "type": "assistant_done",
                    "device_id": request.device_id,
                    "text": text,
                }
            )

    async def _send_qq_reply(self, request: TextRequest, text: str) -> None:
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
