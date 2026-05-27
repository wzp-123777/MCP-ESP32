from __future__ import annotations

import base64
import json
import logging
import re
from dataclasses import dataclass, field
from typing import Any, AsyncIterator

import httpx
from openai import AsyncOpenAI

from app_config import EmbeddingConfig, ModelConfig


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
            payload = json.loads(candidate)
            if isinstance(payload, dict):
                return payload
        except json.JSONDecodeError:
            continue
    decoder = json.JSONDecoder()
    for index, char in enumerate(text):
        if char != "{":
            continue
        try:
            payload, _ = decoder.raw_decode(text[index:])
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            return payload
    raise ValueError(f"cannot parse JSON from model output: {text[:200]}")


def _merge_extra_kwargs(base: dict[str, Any] | None, updates: dict[str, Any]) -> dict[str, Any]:
    merged = dict(base or {})
    merged.update(updates)
    return merged


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


@dataclass(slots=True)
class ImageEnrichmentPlan:
    should_search: bool
    search_query: str = ""
    focus_question: str = ""
    memory_summary: str = ""
    tags: list[str] = field(default_factory=list)
    confidence: float = 0.5


@dataclass(slots=True)
class ImageEnrichmentResult:
    memory_summary: str
    detailed_summary: str = ""
    tags: list[str] = field(default_factory=list)
    confidence: float = 0.5
    search_used: bool = False


@dataclass(slots=True)
class ImageReplyDecision:
    score: int
    should_reply: bool
    reason: str = ""
    relation: str = ""
    reply_hint: str = ""


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
                logger.debug("模型 %s 因 %s 改用 max_tokens 参数重试", self.config.model, exc)
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

    async def complete_json(
        self,
        messages: list[dict[str, Any]],
        *,
        temperature: float = 0.1,
        max_tokens: int = 800,
        schema: dict[str, Any] | None = None,
        extra_kwargs: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        json_mode_kwargs = _merge_extra_kwargs(extra_kwargs, {"response_format": {"type": "json_object"}})
        try:
            text = await self.complete_text(
                messages,
                temperature=temperature,
                max_tokens=max_tokens,
                extra_kwargs=json_mode_kwargs,
            )
        except Exception as exc:
            logger.debug("模型 %s 不支持或拒绝 JSON mode，改用普通文本模式: %s", self.config.model, exc)
            text = await self.complete_text(
                messages,
                temperature=temperature,
                max_tokens=max_tokens,
                extra_kwargs=extra_kwargs,
            )

        try:
            return _extract_json_object(text)
        except ValueError as parse_exc:
            repair_payload = {
                "original_output": text[:4000],
                "expected_schema": schema or {},
                "rules": [
                    "只输出一个合法 JSON object",
                    "不要 Markdown、代码块、解释文字",
                    "字段缺失时用安全默认值补齐",
                    "布尔值必须是 true 或 false，数字必须是 JSON number",
                ],
            }
            repair_messages = [
                {
                    "role": "system",
                    "content": "你是 JSON 修复器。把用户给出的模型输出修复成合法 JSON object。只输出 JSON。",
                },
                {"role": "user", "content": json.dumps(repair_payload, ensure_ascii=False)},
            ]
            try:
                repaired = await self.complete_text(
                    repair_messages,
                    temperature=0.0,
                    max_tokens=max_tokens,
                    extra_kwargs=json_mode_kwargs,
                )
            except Exception:
                repaired = await self.complete_text(
                    repair_messages,
                    temperature=0.0,
                    max_tokens=max_tokens,
                    extra_kwargs=extra_kwargs,
                )
            try:
                return _extract_json_object(repaired)
            except ValueError as repair_exc:
                raise ValueError(
                    f"cannot parse JSON from model output after repair: {parse_exc}; "
                    f"repair_error={repair_exc}; output={text[:200]}"
                ) from repair_exc

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

    def _build_reply_messages(
        self,
        *,
        source: str,
        user_text: str,
        subconscious: str = "",
        tool_context: str = "",
        vision_context: str = "",
        response_style: str = "简洁、自然、口语化。",
    ) -> list[dict[str, str]]:
        system_prompt = (
            "你是代号'小乐'的机器人中枢语言模型。"
            "性格可以偏可爱活泼一点。"
            "请先理解上下文，再直接给出最终回复。"
            "回复默认使用中文，保持短句、自然、清晰。"
            "禁止使用 emoji、表情符号、颜文字、Markdown 或特殊装饰符号；只输出适合语音朗读的中文纯文本。"
            "如果看到了视觉上下文或记忆摘要，把它们当内部线索重新组织表达，不要逐字照抄，也不要暴露'视觉上下文'、'记忆注入'、'摘要'这类内部说法。"
            "如果图像信息还不够完整，要诚实地说还需要再看，不要假装已经确认。"
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
        return [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": "\n\n".join(user_sections)},
        ]

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
            logger.warning("语义规划失败，改用兜底逻辑: %s", exc)
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
        messages = self._build_reply_messages(
            source=source,
            user_text=user_text,
            subconscious=subconscious,
            tool_context=tool_context,
            vision_context=vision_context,
            response_style=response_style,
        )
        yielded = False
        try:
            async for chunk in self._client.stream_text(messages, temperature=0.5, max_tokens=900):
                yielded = True
                yield chunk
        except Exception as exc:
            logger.warning("流式回复失败，改用非流式回复: %s", exc)
        if not yielded:
            text = await self._client.complete_text(messages, temperature=0.5, max_tokens=900)
            if text.strip():
                yield text

    async def compose_image_followup(
        self,
        *,
        user_text: str,
        memory_summary: str,
    ) -> str:
        system_prompt = (
            "你是 QQ 图片补全结果整理器。"
            "用户刚才追问了一张图片的细节，现在后台识别和知识补全已经完成。"
            "请基于内部记忆生成一条最终回复。"
            "用尽量简洁干练，贴合上下文语境的内容去回复精确图文信息的请求。"
            "不要提模型、后台任务、视觉摘要、记忆注入。"
            "不要逐字照抄内部摘要。"
            "如果结论仍不完全确定，要明确使用'像是'、'看起来像'、'大概率是'。"
        )
        user_prompt = json.dumps(
            {
                "user_text": user_text,
                "memory_summary": memory_summary,
                "style_hint": "尽量简洁干练，贴合当前对话语境，优先回答用户真正想确认的图文信息。",
            },
            ensure_ascii=False,
        )
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                temperature=0.35,
                max_tokens=220,
            )
            cleaned = text.strip()
            if cleaned:
                return cleaned
            raise ValueError("empty image follow-up")
        except Exception as exc:
            logger.warning("图片追问生成失败，改用兜底文案: %s", exc)
            return f"噢~我知道了，看起来这张图里是：{memory_summary}"

    async def assess_image_reply_relevance(
        self,
        *,
        source: str,
        user_text: str,
        image_summary: str,
        conversation_context: str = "",
        subconscious: str = "",
        score_threshold: int = 0,
    ) -> ImageReplyDecision:
        system_prompt = (
            "你是图片回复关联评分器。"
            "请判断用户这句话与当前这张图片是否高度相关，图片内容是否应该参与本轮回复。"
            "只输出 JSON，不要解释。"
        )
        user_payload = {
            "source": source,
            "user_text": user_text,
            "image_summary": image_summary,
            "conversation_context": conversation_context[:1800],
            "subconscious": subconscious[:1800],
            "threshold": score_threshold,
            "schema": {
                "score": "0-100 int",
                "should_reply": "bool",
                "reason": "string",
                "relation": "string",
                "reply_hint": "string",
            },
        }
        try:
            payload = await self._client.complete_json(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=220,
                schema=user_payload["schema"],
            )
            score = int(payload.get("score") or 0)
            score = max(0, min(100, score))
            should_reply = bool(payload.get("should_reply", score >= score_threshold))
            if score < score_threshold:
                should_reply = False
            return ImageReplyDecision(
                score=score,
                should_reply=should_reply,
                reason=str(payload.get("reason") or "").strip(),
                relation=str(payload.get("relation") or "").strip(),
                reply_hint=str(payload.get("reply_hint") or "").strip(),
            )
        except Exception as exc:
            logger.warning("图片回复相关性判断失败，改用兜底规则: %s", exc)
            return self._fallback_image_reply_relevance(
                user_text=user_text,
                image_summary=image_summary,
                score_threshold=score_threshold,
            )

    def _fallback_image_reply_relevance(
        self,
        *,
        user_text: str,
        image_summary: str,
        score_threshold: int,
    ) -> ImageReplyDecision:
        normalized = re.sub(r"\s+", "", (user_text or "").strip().lower())
        summary = (image_summary or "").strip()
        direct_markers = (
            "图片",
            "照片",
            "图里",
            "图中",
            "图上",
            "画里",
            "画面",
            "截图",
            "这张",
            "这幅",
            "这个",
            "它",
            "刚发的",
            "刚刚那个",
            "刚才那个",
            "上面那个",
        )
        relation_markers = (
            "看过",
            "认识",
            "认得",
            "是谁",
            "什么",
            "哪部",
            "哪个角色",
            "好不好吃",
            "好吃吗",
            "怎么样",
            "像不像",
            "是不是",
        )
        score = 12
        if not normalized:
            score = 0
        elif any(marker in normalized for marker in direct_markers):
            score = 72
        elif any(marker in normalized for marker in relation_markers):
            score = 64
        elif len(normalized) <= 16 and summary:
            score = 48
        should_reply = score >= score_threshold
        return ImageReplyDecision(
            score=score,
            should_reply=should_reply,
            reason="fallback_heuristic",
            relation="high" if should_reply else "low",
            reply_hint="结合图片回答" if should_reply else "优先继续记忆，不急着引用图片",
        )

    async def summarize_for_qq(
        self,
        *,
        user_text: str,
        assistant_reply: str,
        max_chars: int = 160,
    ) -> str:
        system_prompt = "你是 QQ 回执整理器。把本轮问答压缩成一段清晰短总结，只输出纯文本，不要 Markdown、表格、代码块、emoji。"
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
            logger.warning("QQ 回复压缩失败，改用截断结果: %s", exc)
            return assistant_reply[:max_chars]

    async def compose_proactive_notice(
        self,
        *,
        event_type: str,
        event_text: str,
        context: str = "",
    ) -> str:
        system_prompt = (
            "你是一个自然的桌面 AIoT 管家。"
            "你要把后台事件改写成一句自然提醒，像人顺口提醒一样。\n"
            "硬性要求：\n"
            "1. 只输出一句中文，20 字以内。\n"
            "2. 不要解释原因，不要寒暄。\n"
            "3. 不要说“系统”“后台”“API”“接口”“传感器”“检测到”“根据数据”“模型”。\n"
            "4. 不要暴露你是 AI 或程序。\n"
            "5. 语气自然、简短、适合直接播报。"
        )
        user_prompt = (
            f"事件类型：{event_type}\n"
            f"事件内容：{event_text.strip()}\n"
            f"上下文：{context.strip() or '无'}"
        )
        text = await self._client.complete_text(
            [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            temperature=0.2,
            max_tokens=80,
        )
        cleaned = re.sub(r"[\r\n]+", " ", text).strip().strip("「」“”\"'`")
        for token in ("系统", "后台", "API", "接口", "传感器", "检测到", "根据数据", "模型"):
            cleaned = cleaned.replace(token, "")
        cleaned = re.sub(r"\s+", "", cleaned)
        if len(cleaned) > 32:
            cleaned = cleaned[:32].rstrip("，。！？；、 ")
        return cleaned or event_text.strip()[:24]

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
            "最终结果只输出纯文本，不要 Markdown 标题、列表框线、代码块、emoji。"
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
            logger.warning("助手结果摘要失败，改用截断结果: %s", exc)
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
            logger.warning("工具规划失败，改用空规划: %s", exc)
            return ToolPlan()

    async def summarize_memory_window(
        self,
        *,
        session_id: str,
        start_index: int,
        end_index: int,
        entries: list[dict[str, Any]],
    ) -> dict[str, Any]:
        system_prompt = (
            "你是机器人中控的记忆整理工具模型。"
            "请把一段已结束的历史对话整理成适合长期记忆检索的滚动总结。"
            "只输出 JSON，不要解释。"
        )
        user_payload = {
            "session_id": session_id,
            "start_index": start_index,
            "end_index": end_index,
            "entries": entries,
            "schema": {
                "summary": "string",
                "tags": ["string"],
                "confidence": "0-1 float",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=420,
            )
            payload = _extract_json_object(text)
            return {
                "summary": str(payload.get("summary") or "").strip(),
                "tags": list(payload.get("tags") or []),
                "confidence": float(payload.get("confidence") or 0.5),
            }
        except Exception as exc:
            logger.warning("记忆窗口总结失败，改用兜底整理: %s", exc)
            compact_parts: list[str] = []
            for entry in entries[:8]:
                user_text = str(entry.get("user_text") or "").strip()
                assistant_text = str(entry.get("assistant_text") or "").strip()
                summary = str(entry.get("summary") or "").strip()
                if user_text or assistant_text:
                    compact_parts.append(f"问:{user_text} 答:{assistant_text}".strip())
                elif summary:
                    compact_parts.append(f"观察:{summary}")
            fallback_summary = "；".join(part for part in compact_parts if part)[:320]
            return {
                "summary": fallback_summary,
                "tags": ["fallback_memory"],
                "confidence": 0.2,
            }

    async def extract_structured_memory(
        self,
        *,
        session_id: str,
        entries: list[dict[str, Any]],
        summary: str,
    ) -> dict[str, Any]:
        system_prompt = (
            "你是机器人中控的结构化记忆抽取器。"
            "请从已结束的对话窗口中提炼两类内容："
            "一类是当前会话里仍可能有用的结构化记忆；"
            "另一类是更稳定、适合长期保留的用户画像。"
            "用户画像必须克制，只保留相对稳定的偏好、风格、设备习惯、常驻身份线索。"
            "不要把一次性情绪、临时安排误写成永久画像。"
            "只输出 JSON。"
        )
        user_payload = {
            "session_id": session_id,
            "entries": entries,
            "summary": summary,
            "schema": {
                "memories": [
                    {
                        "type": "task|constraint|preference|temporal_state|vision_fact|topic",
                        "text": "string",
                        "confidence": "0-1 float",
                        "freshness": "short|long|permanent",
                        "valid_until": "optional iso timestamp or empty",
                        "tags": ["string"],
                        "evidence_doc_ids": ["string"],
                    }
                ],
                "profile": [
                    {
                        "key": "string",
                        "value": "string",
                        "summary": "string",
                        "confidence": "0-1 float",
                        "tags": ["string"],
                        "evidence_doc_ids": ["string"],
                    }
                ],
                "summary": "string",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=700,
            )
            payload = _extract_json_object(text)
            memories: list[dict[str, Any]] = []
            for item in list(payload.get("memories") or []):
                text_value = str(item.get("text") or "").strip()
                memory_type = str(item.get("type") or "").strip()
                if not text_value or not memory_type:
                    continue
                memories.append(
                    {
                        "type": memory_type,
                        "text": text_value,
                        "confidence": float(item.get("confidence") or 0.0),
                        "freshness": str(item.get("freshness") or "long").strip() or "long",
                        "valid_until": str(item.get("valid_until") or "").strip(),
                        "tags": [str(tag).strip() for tag in (item.get("tags") or []) if str(tag).strip()],
                        "evidence_doc_ids": [str(doc).strip() for doc in (item.get("evidence_doc_ids") or []) if str(doc).strip()],
                    }
                )
            profile: list[dict[str, Any]] = []
            for item in list(payload.get("profile") or []):
                key = str(item.get("key") or "").strip()
                value = str(item.get("value") or "").strip()
                summary_text = str(item.get("summary") or "").strip()
                if not key or not value or not summary_text:
                    continue
                profile.append(
                    {
                        "key": key,
                        "value": value,
                        "summary": summary_text,
                        "confidence": float(item.get("confidence") or 0.0),
                        "tags": [str(tag).strip() for tag in (item.get("tags") or []) if str(tag).strip()],
                        "evidence_doc_ids": [str(doc).strip() for doc in (item.get("evidence_doc_ids") or []) if str(doc).strip()],
                    }
                )
            return {
                "memories": memories,
                "profile": profile,
                "summary": str(payload.get("summary") or "").strip(),
            }
        except Exception as exc:
            logger.warning("结构化记忆抽取失败，改用规则兜底: %s", exc)
            lowered_summary = summary.strip()
            user_texts = [str(item.get("user_text") or "").strip() for item in entries if str(item.get("user_text") or "").strip()]
            merged_user_text = "\n".join(user_texts)
            profile: list[dict[str, Any]] = []
            if any(token in f"{lowered_summary}\n{merged_user_text}" for token in ("简洁", "干练", "贴合上下文", "精确图文")):
                profile.append(
                    {
                        "key": "reply_style",
                        "value": "偏好简洁干练",
                        "summary": "用户偏好简洁干练、贴题的回复风格。",
                        "confidence": 0.78,
                        "tags": ["style_preference"],
                        "evidence_doc_ids": [],
                    }
                )
            gender_match = re.search(r"(?:其实)?(?:我)?是(?P<gender>男孩子|男生|女生|女孩子)", merged_user_text)
            if gender_match is not None:
                gender = gender_match.group("gender")
                normalized_gender = "男生" if "男" in gender else "女生"
                profile.append(
                    {
                        "key": "gender_identity",
                        "value": normalized_gender,
                        "summary": f"用户曾明确说明自己是{normalized_gender}。",
                        "confidence": 0.92,
                        "tags": ["explicit_self_identification"],
                        "evidence_doc_ids": [],
                    }
                )
            memories: list[dict[str, Any]] = []
            if lowered_summary:
                memories.append(
                    {
                        "type": "topic",
                        "text": lowered_summary[:180],
                        "confidence": 0.35,
                        "freshness": "long",
                        "valid_until": "",
                        "tags": ["fallback_structured_memory"],
                        "evidence_doc_ids": [],
                    }
                )
            if any(token in merged_user_text for token in ("不要180秒", "十轮对话", "10分钟", "十分钟")):
                memories.append(
                    {
                        "type": "preference",
                        "text": "用户希望时间相关策略更贴近短间隔交流，倾向约十分钟断档、十轮左右的对话尺度。",
                        "confidence": 0.78,
                        "freshness": "long",
                        "valid_until": "",
                        "tags": ["timing_preference"],
                        "evidence_doc_ids": [],
                    }
                )
            return {
                "memories": memories,
                "profile": profile,
                "summary": lowered_summary[:140],
            }

    async def analyze_offline_gap(
        self,
        *,
        session_id: str,
        source: str,
        gap_minutes: float,
        pre_gap_entries: list[dict[str, Any]],
        pre_gap_summaries: list[dict[str, Any]],
        return_user_text: str,
        return_assistant_text: str,
        last_turn_timestamp: str,
        now_timestamp: str,
    ) -> dict[str, Any]:
        system_prompt = (
            "你是对话时间断层分析器。"
            "目标不是直接回复用户，而是评估用户离线期间可能发生的生活/任务变化，并根据回归后的首轮对话做拟合。"
            "请明确区分事实、推断、以及适合未来 system prompt 注入的候选提示。"
            "推断必须克制，避免过度脑补。"
            "只输出 JSON。"
        )
        user_payload = {
            "session_id": session_id,
            "source": source,
            "gap_minutes": round(gap_minutes, 2),
            "last_turn_timestamp": last_turn_timestamp,
            "now_timestamp": now_timestamp,
            "pre_gap_entries": pre_gap_entries,
            "pre_gap_summaries": pre_gap_summaries,
            "return_turn": {
                "user_text": return_user_text,
                "assistant_text": return_assistant_text,
            },
            "schema": {
                "pre_gap_state": "string",
                "likely_offline_activities": ["string"],
                "likely_new_constraints": ["string"],
                "return_fit": "string",
                "prompt_injection_preview": "string",
                "log_summary": "string",
                "confidence": "0-1 float",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.15,
                max_tokens=520,
            )
            payload = _extract_json_object(text)
            return {
                "pre_gap_state": str(payload.get("pre_gap_state") or "").strip(),
                "likely_offline_activities": [str(item).strip() for item in (payload.get("likely_offline_activities") or []) if str(item).strip()],
                "likely_new_constraints": [str(item).strip() for item in (payload.get("likely_new_constraints") or []) if str(item).strip()],
                "return_fit": str(payload.get("return_fit") or "").strip(),
                "prompt_injection_preview": str(payload.get("prompt_injection_preview") or "").strip(),
                "log_summary": str(payload.get("log_summary") or "").strip(),
                "confidence": float(payload.get("confidence") or 0.5),
            }
        except Exception as exc:
            logger.warning("离线期分析失败，改用规则兜底: %s", exc)
            recent_user_topics = [str(item.get("user_text") or "").strip() for item in pre_gap_entries[-3:] if str(item.get("user_text") or "").strip()]
            topic_hint = "；".join(recent_user_topics)[:120]
            return {
                "pre_gap_state": f"离线前最近在聊：{topic_hint}" if topic_hint else "离线前对话主题不明显。",
                "likely_offline_activities": ["用户可能去处理现实事务、休息、进食、通勤或工作学习。"],
                "likely_new_constraints": ["重新上线后，用户的关注点、紧迫度和情绪可能已变化。"],
                "return_fit": f"回归后的第一句是“{return_user_text[:80]}”，适合用它重新校准当前上下文。",
                "prompt_injection_preview": "距上一轮已过去一段时间，请先考虑用户状态、任务紧迫度和环境可能已变化，再组织回应。",
                "log_summary": f"离线约 {gap_minutes:.1f} 分钟；建议把回归首句当成新的状态校准信号。",
                "confidence": 0.25,
            }

    async def build_returning_user_context(
        self,
        *,
        session_id: str,
        source: str,
        gap_minutes: float,
        last_turn_timestamp: str,
        now_timestamp: str,
        current_user_text: str,
        recent_entries: list[dict[str, Any]],
        recent_summaries: list[dict[str, Any]],
        profile_facts: list[str],
        active_memories: list[str],
    ) -> dict[str, Any]:
        system_prompt = (
            "你是首轮时间感知上下文整理器。"
            "目标不是直接回复用户，而是为接下来这一轮对话生成一段内部提示。"
            "请结合时间间隔、最近对话、稳定用户画像、结构化记忆，"
            "用克制、简短、可执行的方式描述本轮应该如何理解用户当前状态。"
            "不要过度脑补，不要把推断写成事实。"
            "只输出 JSON。"
        )
        user_payload = {
            "session_id": session_id,
            "source": source,
            "gap_minutes": round(gap_minutes, 2),
            "last_turn_timestamp": last_turn_timestamp,
            "now_timestamp": now_timestamp,
            "current_user_text": current_user_text,
            "recent_entries": recent_entries,
            "recent_summaries": recent_summaries,
            "profile_facts": profile_facts,
            "active_memories": active_memories,
            "schema": {
                "prompt_block": "string",
                "reply_strategy": "string",
                "time_consideration": "string",
                "log_summary": "string",
                "confidence": "0-1 float",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.12,
                max_tokens=420,
            )
            payload = _extract_json_object(text)
            return {
                "prompt_block": str(payload.get("prompt_block") or "").strip(),
                "reply_strategy": str(payload.get("reply_strategy") or "").strip(),
                "time_consideration": str(payload.get("time_consideration") or "").strip(),
                "log_summary": str(payload.get("log_summary") or "").strip(),
                "confidence": float(payload.get("confidence") or 0.45),
            }
        except Exception as exc:
            logger.warning("首轮时间上下文整理失败，改用规则兜底: %s", exc)
            recent_user_topics = [
                str(item.get("user_text") or "").strip()
                for item in recent_entries[-3:]
                if str(item.get("user_text") or "").strip()
            ]
            topic_hint = "；".join(recent_user_topics)[:120]
            profile_hint = "；".join(profile_facts[:3])[:120]
            memory_hint = "；".join(active_memories[:3])[:120]
            lines = [
                "时间感知回归提示:",
                f"- 距上一轮约 {gap_minutes:.1f} 分钟，本轮应先按当前这句话重新校准状态。",
                f"- 上次交互时间: {last_turn_timestamp}",
                f"- 当前时间: {now_timestamp}",
            ]
            if topic_hint:
                lines.append(f"- 离线前最近在聊: {topic_hint}")
            if profile_hint:
                lines.append(f"- 可参考的稳定用户画像: {profile_hint}")
            if memory_hint:
                lines.append(f"- 可参考的结构化记忆: {memory_hint}")
            lines.append("- 回复时要考虑用户的场景、紧迫度和情绪可能已变化，但推断必须克制。")
            return {
                "prompt_block": "\n".join(lines),
                "reply_strategy": "把当前用户输入当成断档后的重新校准信号，优先回答此刻最相关的内容。",
                "time_consideration": "先考虑时间断档带来的场景变化，再利用历史记忆补足稳定背景。",
                "log_summary": f"离线约 {gap_minutes:.1f} 分钟；已按时间断档模式处理首轮消息。",
                "confidence": 0.25,
            }

    async def polish_image_followup(
        self,
        *,
        user_text: str,
        memory_summary: str,
        max_chars: int = 140,
    ) -> str:
        system_prompt = (
            "你是 QQ 图片回复润色器。"
            "请把内部图片记忆整理成一条最终回复。"
            "用尽量简洁干练，贴合上下文语境的内容去回复精确图文信息的请求。"
            "不要直接照抄原始摘要，不要写成说明书，不要出现“图片展示”“手机屏幕显示”这类生硬起手。"
            "允许保留关键事实，但优先短、准、贴题。"
            "只输出最终回复文本。"
        )
        user_prompt = json.dumps(
            {
                "user_text": user_text,
                "memory_summary": memory_summary,
                "constraint": f"中文，尽量控制在 {max_chars} 字以内，优先回答图里最关键的可确认信息。",
            },
            ensure_ascii=False,
        )
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ],
                temperature=0.25,
                max_tokens=220,
            )
            return text.strip()
        except Exception as exc:
            logger.warning("图片追问润色失败，跳过润色步骤: %s", exc)
            return ""

    async def plan_image_enrichment(
        self,
        *,
        scene_summary: str,
        highres_answer: str = "",
        caption: str = "",
    ) -> ImageEnrichmentPlan:
        system_prompt = (
            "你是机器人视觉记忆补全规划器。"
            "你要判断图片内容是否值得联网补全角色名、IP名、地点名、品牌名、作品名。"
            "只输出 JSON。"
        )
        user_payload = {
            "scene_summary": scene_summary,
            "highres_answer": highres_answer,
            "caption": caption,
            "schema": {
                "should_search": "bool",
                "search_query": "string",
                "focus_question": "string",
                "memory_summary": "string",
                "tags": ["string"],
                "confidence": "0-1 float",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=320,
            )
            payload = _extract_json_object(text)
            return ImageEnrichmentPlan(
                should_search=bool(payload.get("should_search")),
                search_query=str(payload.get("search_query") or "").strip(),
                focus_question=str(payload.get("focus_question") or "").strip(),
                memory_summary=str(payload.get("memory_summary") or scene_summary).strip(),
                tags=list(payload.get("tags") or []),
                confidence=float(payload.get("confidence") or 0.5),
            )
        except Exception as exc:
            logger.warning("图片补充规划失败，改用规则兜底: %s", exc)
            return ImageEnrichmentPlan(
                should_search=any(marker in f"{scene_summary} {highres_answer}" for marker in ("卡通", "角色", "玩偶", "动漫", "IP", "logo", "品牌")),
                search_query=(highres_answer or scene_summary)[:96],
                focus_question="请识别这张图中的角色、IP、地点或作品名。",
                memory_summary=scene_summary.strip(),
                tags=["image_memory"],
                confidence=0.35,
            )

    async def finalize_image_enrichment(
        self,
        *,
        scene_summary: str,
        highres_answer: str = "",
        search_result: str = "",
        caption: str = "",
    ) -> ImageEnrichmentResult:
        system_prompt = (
            "你是机器人视觉长期记忆整理器。"
            "请结合图片摘要、高清观察和联网结果，生成适合记忆写入的详细结论。"
            "不要虚构；不确定就明确写疑似、可能。"
            "只输出 JSON。"
        )
        user_payload = {
            "scene_summary": scene_summary,
            "highres_answer": highres_answer,
            "search_result": search_result,
            "caption": caption,
            "schema": {
                "memory_summary": "string",
                "detailed_summary": "string",
                "tags": ["string"],
                "confidence": "0-1 float",
                "search_used": "bool",
            },
        }
        try:
            text = await self._client.complete_text(
                [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": json.dumps(user_payload, ensure_ascii=False)},
                ],
                temperature=0.1,
                max_tokens=420,
            )
            payload = _extract_json_object(text)
            return ImageEnrichmentResult(
                memory_summary=str(payload.get("memory_summary") or scene_summary).strip(),
                detailed_summary=str(payload.get("detailed_summary") or "").strip(),
                tags=list(payload.get("tags") or []),
                confidence=float(payload.get("confidence") or 0.5),
                search_used=bool(payload.get("search_used")),
            )
        except Exception as exc:
            logger.warning("图片补充整合失败，改用直接拼接: %s", exc)
            merged = "；".join(part for part in (scene_summary.strip(), highres_answer.strip(), search_result.strip()) if part)
            return ImageEnrichmentResult(
                memory_summary=(search_result or highres_answer or scene_summary).strip()[:220],
                detailed_summary=merged[:420],
                tags=["image_memory", "fallback_enrichment"],
                confidence=0.3,
                search_used=bool(search_result.strip()),
            )


class ContextEmbeddingService:
    def __init__(self, config: EmbeddingConfig) -> None:
        self.config = config
        self.model_name = config.model
        self.dimension = max(128, int(config.dimension))

    @property
    def enabled(self) -> bool:
        return bool(self.config.api_key and self.config.endpoint and self.model_name)

    async def embed_text(self, text: str) -> dict[str, Any]:
        result = await self.embed_texts([text])
        vectors = result.get("vectors") or []
        return {
            "embedding": vectors[0] if vectors else [],
            "request_id": result.get("request_id"),
            "usage": result.get("usage"),
        }

    async def embed_texts(self, texts: list[str]) -> dict[str, Any]:
        normalized = [text.strip() for text in texts if text and text.strip()]
        if not normalized:
            return {"vectors": [], "request_id": "", "usage": {}}
        if not self.enabled:
            raise RuntimeError("context embedding service is not configured")

        all_vectors: list[list[float]] = []
        request_ids: list[str] = []
        usage_total = {"total_tokens": 0}
        for start in range(0, len(normalized), 20):
            batch = normalized[start : start + 20]
            payload = {
                "model": self.model_name,
                "input": {
                    "contents": [{"text": item} for item in batch],
                },
                "parameters": {
                    "dimension": self.dimension,
                    "output_type": "dense",
                },
            }
            headers = {
                "Authorization": f"Bearer {self.config.api_key}",
                "Content-Type": "application/json",
            }
            async with httpx.AsyncClient(timeout=self.config.timeout_seconds) as client:
                response = await client.post(self.config.endpoint, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            if str(data.get("code") or "200") != "200":
                raise RuntimeError(f"embedding api failed: {data}")

            output = data.get("output") or {}
            items = list(output.get("embeddings") or [])
            items.sort(key=lambda item: int(item.get("index") or 0))
            for item in items:
                vector = [float(value) for value in (item.get("embedding") or [])]
                all_vectors.append(vector)

            usage = data.get("usage") or {}
            usage_total["total_tokens"] += int(usage.get("total_tokens") or 0)
            request_id = str(data.get("request_id") or "")
            if request_id:
                request_ids.append(request_id)

        return {
            "vectors": all_vectors,
            "request_id": ",".join(request_ids),
            "usage": usage_total,
        }


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
            logger.warning("视觉分析失败，改用兜底描述: %s", exc)
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
            logger.warning("高清视觉分析失败，改用兜底结果: %s", exc)
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
    def __init__(self, config: ModelConfig, voice: str, style_prompt: str = "") -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model
        self.voice = voice
        self.style_prompt = style_prompt.strip()

    async def stream_audio(self, text: str) -> AsyncIterator[dict[str, Any]]:
        cleaned = text.strip()
        if not cleaned:
            return
        if self._is_mimo_tts_model():
            audio = await self._complete_mimo_audio_non_stream(cleaned)
            if not audio:
                logger.warning("MiMo TTS 非流式没有返回音频，尝试流式兜底")
                audio = await self._complete_mimo_audio_stream(cleaned)
            if audio:
                yield audio
            return
        else:
            messages = [
                {"role": "system", "content": self._tts_system_prompt()},
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

    async def _complete_mimo_audio_non_stream(self, text: str) -> dict[str, Any] | None:
        messages = self._mimo_tts_messages(text)
        extra_kwargs = {
            "modalities": ["audio"],
            "audio": self._mimo_audio_options("wav"),
        }
        response = await self._client._create_chat_completion(
            messages=messages,
            stream=False,
            temperature=0.0,
            **extra_kwargs,
        )
        choices = _safe_get(response, "choices", []) or []
        if not choices:
            return None
        message = _safe_get(choices[0], "message")
        audio_obj = _safe_get(message, "audio")
        audio_b64 = str(_safe_get(audio_obj, "data", "") or "")
        audio_bytes = self._decode_audio_b64(audio_b64)
        wav = self._extract_complete_wav(audio_bytes)
        if not wav:
            logger.warning("MiMo TTS 非流式音频不是完整 WAV: bytes=%d", len(audio_bytes))
            return None
        return {
            "audio_b64": base64.b64encode(wav).decode("ascii"),
            "transcript": _safe_get(audio_obj, "transcript", ""),
            "audio_id": _safe_get(audio_obj, "id", ""),
            "format": _safe_get(audio_obj, "format", "wav") or "wav",
            "voice": self._effective_voice_label(),
        }

    async def _complete_mimo_audio_stream(self, text: str) -> dict[str, Any] | None:
        messages = self._mimo_tts_messages(text)
        extra_kwargs = {
            "modalities": ["audio"],
            "audio": self._mimo_audio_options("wav"),
        }
        stream = await self._client._create_chat_completion(
            messages=messages,
            stream=True,
            temperature=0.0,
            **extra_kwargs,
        )
        chunks_by_id: dict[str, list[bytes]] = {}
        meta_by_id: dict[str, dict[str, Any]] = {}
        id_order: list[str] = []
        ordered_audio: list[bytes] = []
        last_audio_meta: dict[str, Any] = {"transcript": "", "audio_id": "", "format": "wav", "voice": self._effective_voice_label()}
        async for chunk in stream:
            choices = _safe_get(chunk, "choices", []) or []
            if not choices:
                continue
            delta = _safe_get(choices[0], "delta")
            audio_delta = _safe_get(delta, "audio")
            if not audio_delta:
                continue
            audio_data = _safe_get(audio_delta, "data")
            if not audio_data:
                continue
            audio_bytes = self._decode_audio_b64(str(audio_data))
            if not audio_bytes:
                continue
            audio_id = str(_safe_get(audio_delta, "id", "") or "")
            last_audio_meta = {
                "transcript": _safe_get(audio_delta, "transcript", ""),
                "audio_id": audio_id,
                "format": _safe_get(audio_delta, "format", "wav") or "wav",
                "voice": self._effective_voice_label(),
            }
            if audio_id:
                if audio_id not in chunks_by_id:
                    chunks_by_id[audio_id] = []
                    id_order.append(audio_id)
                chunks_by_id[audio_id].append(audio_bytes)
                meta_by_id[audio_id] = last_audio_meta
            else:
                ordered_audio.append(audio_bytes)
        if id_order:
            audio_id = id_order[-1]
            audio_b64 = self._pick_mimo_audio_payload(chunks_by_id[audio_id])
            return {
                "audio_b64": audio_b64,
                **meta_by_id[audio_id],
            }
        if ordered_audio:
            audio_b64 = self._pick_mimo_audio_payload(ordered_audio)
            return {
                "audio_b64": audio_b64,
                **last_audio_meta,
            }
        return None

    def _decode_audio_b64(self, audio_b64: str) -> bytes:
        compact = re.sub(r"\s+", "", audio_b64 or "")
        if not compact:
            return b""
        try:
            return base64.b64decode(compact, validate=False)
        except Exception:
            return b""

    def _pick_mimo_audio_payload(self, chunks: list[bytes] | Any) -> str:
        chunk_list = list(chunks)
        for chunk in reversed(chunk_list):
            audio = bytes(chunk)
            wav = self._extract_complete_wav(audio)
            if wav:
                return base64.b64encode(wav).decode("ascii")
        joined = b"".join(bytes(chunk) for chunk in chunk_list)
        wav = self._extract_complete_wav(joined)
        if wav:
            return base64.b64encode(wav).decode("ascii")
        return base64.b64encode(joined).decode("ascii")

    def _extract_complete_wav(self, audio: bytes) -> bytes:
        if not audio:
            return b""
        index = audio.rfind(b"RIFF")
        while index >= 0:
            if index + 12 <= len(audio) and audio[index + 8 : index + 12] == b"WAVE":
                candidate = audio[index:]
                if len(candidate) >= 44:
                    declared = int.from_bytes(candidate[4:8], "little", signed=False) + 8
                    if 44 <= declared <= len(candidate):
                        wav = candidate[:declared]
                        if b"fmt " in wav[12: min(len(wav), 256)] and b"data" in wav[12:]:
                            return wav
            index = audio.rfind(b"RIFF", 0, index)
        return b""

    def _looks_like_wav_b64(self, audio_b64: str) -> bool:
        audio = self._decode_audio_b64(audio_b64)
        if len(audio) < 44 or audio[:4] != b"RIFF" or audio[8:12] != b"WAVE":
            return False
        declared = int.from_bytes(audio[4:8], "little", signed=False) + 8
        if declared < 44 or declared > len(audio):
            return False
        return b"fmt " in audio[12: min(len(audio), 256)] and b"data" in audio[12:]

    def _is_mimo_tts_model(self) -> bool:
        return "mimo" in (self.model_name or "").lower() and "tts" in (self.model_name or "").lower()

    def _is_mimo_voice_design_model(self) -> bool:
        return "voicedesign" in (self.model_name or "").lower()

    def _mimo_audio_options(self, audio_format: str) -> dict[str, str]:
        audio = {"format": audio_format}
        if not self._is_mimo_voice_design_model() and self.voice:
            audio["voice"] = self.voice
        return audio

    def _effective_voice_label(self) -> str:
        if self._is_mimo_voice_design_model():
            return "voicedesign"
        return self.voice

    def _tts_system_prompt(self) -> str:
        base = (
            "你是语音合成模型。请严格朗读待合成的中文纯文本，不要补充、不改写；"
            "如果文本里残留 emoji、表情符号或颜文字，直接忽略，不要读出它们的名称。"
        )
        if not self.style_prompt:
            return base
        return f"{base}\n音色与朗读风格要求：{self.style_prompt}"

    def _mimo_tts_messages(self, text: str) -> list[dict[str, str]]:
        messages: list[dict[str, str]] = []
        if self.style_prompt:
            messages.append({"role": "user", "content": self.style_prompt})
        elif self._is_mimo_voice_design_model():
            messages.append(
                {
                    "role": "user",
                    "content": "Warm, clear, natural conversational voice with moderate speed and friendly tone.",
                }
            )
        messages.append({"role": "assistant", "content": text})
        return messages


class ASRModelService:
    def __init__(self, config: ModelConfig, *, language: str = "zh") -> None:
        self._client = OpenAICompatibleChatClient(config)
        self.model_name = config.model
        self.language = language or "zh"

    async def transcribe_wav(
        self,
        wav_bytes: bytes,
        *,
        mime_type: str = "audio/wav",
        prompt: str = "",
    ) -> dict[str, Any]:
        if not wav_bytes:
            return {"text": "", "model": self.model_name, "language": self.language}
        audio_b64 = base64.b64encode(wav_bytes).decode("ascii")
        data_uri = f"data:{mime_type};base64,{audio_b64}"
        messages = [
            {
                "role": "user",
                "content": [
                    {"type": "input_audio", "input_audio": {"data": data_uri}},
                ],
            },
        ]
        extra_kwargs = {
            "extra_body": {
                "asr_options": {
                    "language": self.language,
                    "enable_itn": True,
                }
            }
        }
        text = await self._client.complete_text(
            messages,
            extra_kwargs=extra_kwargs,
        )
        return {
            "text": text.strip(),
            "model": self.model_name,
            "language": self.language,
        }
