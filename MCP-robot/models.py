from __future__ import annotations

import json
import logging
import re
from dataclasses import dataclass, field
from typing import Any, AsyncIterator

from openai import AsyncOpenAI

from app_config import ModelConfig


logger = logging.getLogger("MCP_Robot.Models")


def _safe_get(obj: Any, name: str, default: Any = None) -> Any:
    if obj is None:
        return default
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


def _extract_text_from_content(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for item in content:
            if isinstance(item, dict):
                if item.get("type") == "text":
                    parts.append(item.get("text", ""))
                elif "text" in item:
                    parts.append(str(item.get("text", "")))
            else:
                text = _safe_get(item, "text", "")
                if text:
                    parts.append(str(text))
        return "".join(parts)
    return str(content or "")


def _extract_json_object(text: str) -> dict[str, Any]:
    text = text.strip()
    candidates = [text]
    fence = re.findall(r"```(?:json)?\s*([\s\S]*?)```", text)
    candidates.extend(fence)
    brace_match = re.search(r"\{[\s\S]*\}", text)
    if brace_match:
        candidates.append(brace_match.group(0))
    for candidate in candidates:
        try:
            return json.loads(candidate)
        except json.JSONDecodeError:
            continue
    raise ValueError(f"cannot parse JSON from model output: {text[:200]}")


@dataclass(slots=True)
class SemanticPlan:
    intent: str = "chat"
    needs_tools: bool = False
    should_reply: bool = True
    tool_goal: str = ""
    response_style: str = "简洁、自然、口语化。"
    skip_language_reply: bool = False


@dataclass(slots=True)
class ToolPlan:
    calls: list[dict[str, Any]] = field(default_factory=list)
    return_to_language: bool = True
    direct_reply: str = ""
    device_commands: list[dict[str, Any]] = field(default_factory=list)


@dataclass(slots=True)
class VisionAnalysis:
    scene_summary: str
    should_reply: bool
    suggested_reply: str = ""
    confidence: float = 0.5
    importance: float = 0.5
    tags: list[str] = field(default_factory=list)
    device_action: dict[str, Any] | None = None


class OpenAICompatibleChatClient:
    def __init__(self, config: ModelConfig) -> None:
        self.config = config
        self.client = AsyncOpenAI(
            api_key=config.api_key or "EMPTY",
            base_url=config.base_url,
            timeout=config.timeout_seconds,
        )

    async def _create_chat_completion(self, **kwargs: Any) -> Any:
        request = dict(kwargs)
        if "max_tokens" in request and request["max_tokens"] is None:
            request.pop("max_tokens")
        if "max_tokens" in request:
            request["max_completion_tokens"] = request.pop("max_tokens")
        try:
            return await self.client.chat.completions.create(model=self.config.model, **request)
        except Exception as exc:
            if "max_completion_tokens" in request:
                request["max_tokens"] = request.pop("max_completion_tokens")
                logger.debug("Retrying %s with max_tokens after %s", self.config.model, exc)
                return await self.client.chat.completions.create(model=self.config.model, **request)
            raise

    async def complete_text(
        self,
        messages: list[dict[str, Any]],
        *,
        temperature: float = 0.3,
        max_tokens: int = 800,
        extra_kwargs: dict[str, Any] | None = None,
    ) -> str:
        response = await self._create_chat_completion(
            messages=messages,
            stream=False,
            temperature=temperature,
            max_tokens=max_tokens,
            **(extra_kwargs or {}),
        )
        message = response.choices[0].message
        return _extract_text_from_content(_safe_get(message, "content", ""))

    async def stream_text(
        self,
        messages: list[dict[str, Any]],
        *,
        temperature: float = 0.4,
        max_tokens: int = 900,
        extra_kwargs: dict[str, Any] | None = None,
    ) -> AsyncIterator[str]:
        stream = await self._create_chat_completion(
            messages=messages,
            stream=True,
            temperature=temperature,
            max_tokens=max_tokens,
            **(extra_kwargs or {}),
        )
        async for chunk in stream:
            choices = _safe_get(chunk, "choices", []) or []
            if not choices:
                continue
            delta = _safe_get(choices[0], "delta")
            text = _extract_text_from_content(_safe_get(delta, "content", ""))
            if text:
                yield text


class LanguageModelService:
    def __init__(self, config: ModelConfig) -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model

    async def plan_text(self, *, source: str, user_text: str, subconscious: str = "") -> SemanticPlan:
        system_prompt = (
            "你是机器人中控的语义规划器。"
            "请先判断当前消息是否需要调用工具、是否需要回复，并输出 JSON。"
            "不要写解释，只输出 JSON。"
        )
        user_prompt = json.dumps(
            {
                "source": source,
                "user_text": user_text,
                "subconscious": subconscious,
                "schema": {
                    "intent": "string",
                    "needs_tools": "bool",
                    "should_reply": "bool",
                    "tool_goal": "string",
                    "response_style": "string",
                    "skip_language_reply": "bool",
                },
            },
            ensure_ascii=False,
        )
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                temperature=0.1,
                max_tokens=400,
            )
            payload = _extract_json_object(text)
            return SemanticPlan(
                intent=str(payload.get("intent") or "chat"),
                needs_tools=bool(payload.get("needs_tools")),
                should_reply=bool(payload.get("should_reply", True)),
                tool_goal=str(payload.get("tool_goal") or ""),
                response_style=str(payload.get("response_style") or "简洁、自然、口语化。"),
                skip_language_reply=bool(payload.get("skip_language_reply")),
            )
        except Exception as exc:
            logger.warning("Semantic planning failed, using fallback: %s", exc)
            return SemanticPlan(
                needs_tools=any(keyword in user_text.lower() for keyword in ("天气", "weather", "搜索", "search", "导航", "地图")),
                should_reply=True,
                tool_goal=user_text,
            )

    async def stream_reply(
        self,
        *,
        source: str,
        user_text: str,
        subconscious: str = "",
        tool_context: str = "",
        vision_context: str = "",
        response_style: str = "简洁、自然、口语化。",
    ) -> AsyncIterator[str]:
        system_prompt = (
            "你是代号'小乐'的机器人中枢语言模型。"
            "请先理解上下文，再直接给出最终回复。"
            "回复默认使用中文，保持短句、自然、清晰。"
        )
        user_sections = [
            f"消息来源: {source}",
            f"用户输入: {user_text}",
            f"回复风格: {response_style}",
        ]
        if subconscious:
            user_sections.append(subconscious)
        if vision_context:
            user_sections.append(f"视觉上下文:\n{vision_context}")
        if tool_context:
            user_sections.append(tool_context)
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": "\n\n".join(user_sections)},
        ]
        async for chunk in self._client.stream_text(messages, temperature=0.5, max_tokens=900):
            yield chunk

    async def summarize_for_qq(
        self,
        *,
        user_text: str,
        assistant_reply: str,
        max_chars: int = 160,
    ) -> str:
        system_prompt = "你是 QQ 回执整理器。把本轮问答压缩成一段清晰短总结。"
        user_prompt = json.dumps(
            {
                "user_text": user_text,
                "assistant_reply": assistant_reply,
                "constraint": f"用中文，不超过 {max_chars} 个字。",
            },
            ensure_ascii=False,
        )
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                temperature=0.2,
                max_tokens=220,
            )
            return text.strip()
        except Exception as exc:
            logger.warning("QQ summary fallback: %s", exc)
            return assistant_reply[:max_chars]

    async def summarize_agent_result(
        self,
        *,
        command: str,
        raw_result: str,
        max_chars: int = 500,
    ) -> str:
        system_prompt = (
            "你是 root agent 执行结果整理器。"
            "请把执行记录整理成用户可直接阅读的最终结果。"
            "不要输出中间推理、工具调用、脚本代码、日志噪音。"
            "如果任务未完成，要明确说明卡在哪。"
        )
        user_prompt = json.dumps(
            {
                "command": command,
                "raw_result": raw_result,
                "constraint": f"用中文，保留关键信息，尽量控制在 {max_chars} 字以内。",
            },
            ensure_ascii=False,
        )
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                temperature=0.2,
                max_tokens=420,
            )
            return text.strip()
        except Exception as exc:
            logger.warning("Agent summary fallback: %s", exc)
            return raw_result[:max_chars]


class ToolModelService:
    def __init__(self, config: ModelConfig) -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model

    async def plan_tools(
        self,
        *,
        user_text: str,
        semantic_plan: SemanticPlan,
        tool_catalog: list[dict[str, Any]],
        extra_context: str = "",
    ) -> ToolPlan:
        system_prompt = (
            "你是机器人中控的工具调度模型。"
            "根据用户意图与工具目录，输出 JSON 工具计划。"
            "不要虚构不存在的工具名。不要输出解释。"
        )
        user_payload = {
            "user_text": user_text,
            "extra_context": extra_context,
            "semantic_plan": {
                "intent": semantic_plan.intent,
                "tool_goal": semantic_plan.tool_goal,
                "skip_language_reply": semantic_plan.skip_language_reply,
            },
            "tool_catalog": tool_catalog,
            "schema": {
                "calls": [{"name": "string", "arguments": {}}],
                "return_to_language": True,
                "direct_reply": "string",
                "device_commands": [],
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=500,
            )
            payload = _extract_json_object(text)
            return ToolPlan(
                calls=list(payload.get("calls") or []),
                return_to_language=bool(payload.get("return_to_language", True)),
                direct_reply=str(payload.get("direct_reply") or ""),
                device_commands=list(payload.get("device_commands") or []),
            )
        except Exception as exc:
            logger.warning("Tool planning fallback: %s", exc)
            return ToolPlan()


class VisionModelService:
    def __init__(self, config: ModelConfig) -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model

    async def observe_environment(
        self,
        *,
        image_base64: str,
        mime_type: str,
        source_hint: str = "esp32",
        caption: str = "",
    ) -> VisionAnalysis:
        system_prompt = (
            "你是机器人的低频环境观察视觉模块。"
            "当前任务是快速扫一眼环境，写出简短场景摘要，并判断是否需要机器人立刻开口。"
            "只输出 JSON。"
        )
        prompt = json.dumps(
            {
                "source": source_hint,
                "caption": caption,
                "schema": {
                    "scene_summary": "string",
                    "should_reply": "bool",
                    "suggested_reply": "string",
                    "confidence": "0-1 float",
                    "importance": "0-1 float",
                    "tags": ["string"],
                    "device_action": "object or null",
                },
            },
            ensure_ascii=False,
        )
        messages = [
            {"role": "system", "content": system_prompt},
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {"url": f"data:{mime_type};base64,{image_base64}"}},
                ],
            },
        ]
        try:
            text = await self._client.complete_text(messages, temperature=0.1, max_tokens=500)
            payload = _extract_json_object(text)
            return VisionAnalysis(
                scene_summary=str(payload.get("scene_summary") or ""),
                should_reply=bool(payload.get("should_reply")),
                suggested_reply=str(payload.get("suggested_reply") or ""),
                confidence=float(payload.get("confidence") or 0.5),
                importance=float(payload.get("importance") or 0.5),
                tags=list(payload.get("tags") or []),
                device_action=payload.get("device_action"),
            )
        except Exception as exc:
            logger.warning("Vision analysis fallback: %s", exc)
            return VisionAnalysis(
                scene_summary=caption or "视觉输入到达，但解析失败。",
                should_reply=False,
                confidence=0.2,
                importance=0.2,
            )

    async def inspect_high_res(
        self,
        *,
        image_base64: str,
        mime_type: str,
        question: str,
        scene_summary: str = "",
    ) -> dict[str, Any]:
        system_prompt = (
            "你是机器人的高清视觉检查模块。"
            "请仔细查看图片细节，回答用户问题。"
            "输出 JSON。"
        )
        prompt = json.dumps(
            {
                "question": question,
                "scene_summary": scene_summary,
                "schema": {
                    "answer": "string",
                    "confidence": "0-1 float",
                    "key_details": ["string"],
                },
            },
            ensure_ascii=False,
        )
        messages = [
            {"role": "system", "content": system_prompt},
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {"url": f"data:{mime_type};base64,{image_base64}"}},
                ],
            },
        ]
        try:
            text = await self._client.complete_text(messages, temperature=0.1, max_tokens=600)
            payload = _extract_json_object(text)
            return {
                "answer": str(payload.get("answer") or ""),
                "confidence": float(payload.get("confidence") or 0.5),
                "key_details": list(payload.get("key_details") or []),
                "model": self.model_name,
            }
        except Exception as exc:
            logger.warning("High-res vision fallback: %s", exc)
            return {
                "answer": "高清视觉检查失败。",
                "confidence": 0.1,
                "key_details": [],
                "model": self.model_name,
                "error": str(exc),
            }

    async def analyze_image(
        self,
        *,
        image_base64: str,
        mime_type: str,
        source_hint: str = "esp32",
        caption: str = "",
    ) -> VisionAnalysis:
        return await self.observe_environment(
            image_base64=image_base64,
            mime_type=mime_type,
            source_hint=source_hint,
            caption=caption,
        )


class TTSModelService:
    def __init__(self, config: ModelConfig, voice: str) -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model
        self.voice = voice

    async def stream_audio(self, text: str) -> AsyncIterator[dict[str, Any]]:
        cleaned = text.strip()
        if not cleaned:
            return
        messages = [
            {
                "role": "system",
                "content": "你是语音合成模型。请严格朗读用户给出的文本，不要补充、不改写。",
            },
            {"role": "user", "content": cleaned},
        ]
        extra_kwargs = {
            "modalities": ["text", "audio"],
            "audio": {"voice": self.voice, "format": "wav"},
        }
        stream = await self._client._create_chat_completion(
            messages=messages,
            stream=True,
            temperature=0.0,
            max_tokens=500,
            **extra_kwargs,
        )
        async for chunk in stream:
            choices = _safe_get(chunk, "choices", []) or []
            if not choices:
                continue
            delta = _safe_get(choices[0], "delta")
            audio_delta = _safe_get(delta, "audio")
            if not audio_delta:
                continue
            audio_data = _safe_get(audio_delta, "data")
            transcript = _safe_get(audio_delta, "transcript", "")
            audio_id = _safe_get(audio_delta, "id", "")
            if audio_data:
                yield {
                    "audio_b64": audio_data,
                    "transcript": transcript,
                    "audio_id": audio_id,
                    "format": _safe_get(audio_delta, "format", "wav") or "wav",
                    "voice": self.voice,
                }
