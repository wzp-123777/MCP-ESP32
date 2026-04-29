from __future__ import annotations

from typing import Any

import httpx

from tools.base import BaseTool, ToolResult


class DashScopeQuarkSearchTool(BaseTool):
    name = "search.web"
    description = "使用 DashScope 内置联网搜索完成网页检索，并返回整理后的答案。"
    output_schema = {
        "type": "object",
        "properties": {
            "answer": {"type": "string"},
            "tool_messages": {"type": "array"},
            "provider": {"type": "string"},
            "model": {"type": "string"},
            "usage": {"type": "object"},
        },
    }
    background_capable = True
    tags = ("search", "web", "dashscope")
    input_schema = {
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "要联网搜索的问题或关键词"},
            "session_knowledge": {"type": "string", "description": "可选，会话级补充知识"},
        },
        "required": ["query"],
    }

    def __init__(
        self,
        api_key: str,
        agent_id: str = "",
        agent_version: str = "",
        workspace_id: str = "",
        timeout_seconds: float = 60.0,
        model: str = "qwen-plus",
        base_url: str = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        search_strategy: str = "turbo",
    ) -> None:
        self.api_key = api_key
        self.agent_id = agent_id
        self.agent_version = agent_version
        self.workspace_id = workspace_id
        self.timeout_seconds = timeout_seconds
        self.model = model
        self.base_url = base_url.rstrip("/")
        self.search_strategy = search_strategy

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        query = str(arguments.get("query") or "").strip()
        if not self.api_key:
            return ToolResult(self.name, False, None, error="DASHSCOPE_API_KEY / BAILIAN_SEARCH_API_KEY is missing")
        if not query:
            return ToolResult(self.name, False, None, error="query is required")

        session_knowledge = str(arguments.get("session_knowledge") or "").strip()
        try:
            result = await self._search(query=query, session_knowledge=session_knowledge)
        except Exception as exc:
            return ToolResult(self.name, False, None, error=str(exc))
        return ToolResult(self.name, True, result)

    async def _search(self, *, query: str, session_knowledge: str = "") -> dict[str, Any]:
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        prompt = query
        if session_knowledge:
            prompt = f"会话补充信息:\n{session_knowledge}\n\n联网搜索任务:\n{query}"

        payload: dict[str, Any] = {
            "model": self.model,
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "你是联网搜索工具。"
                        "请先使用内置搜索获取最新公开信息，再输出简洁可靠的中文答案。"
                        "如果信息不确定，要明确说明。"
                    ),
                },
                {
                    "role": "user",
                    "content": prompt,
                },
            ],
            "enable_search": True,
            "search_options": {
                "forced_search": True,
                "search_strategy": self.search_strategy,
            },
            "temperature": 0.2,
            "stream": False,
        }

        async with httpx.AsyncClient(timeout=self.timeout_seconds) as client:
            response = await client.post(
                f"{self.base_url}/chat/completions",
                headers=headers,
                json=payload,
            )
            response.raise_for_status()
            data = response.json()

        choices = data.get("choices") or []
        if not choices:
            raise RuntimeError("DashScope search returned no choices")
        message = choices[0].get("message") or {}
        content = message.get("content") or ""
        if isinstance(content, list):
            answer = "".join(str(item.get("text") or item.get("content") or "") if isinstance(item, dict) else str(item) for item in content).strip()
        else:
            answer = str(content).strip()
        if not answer:
            raise RuntimeError("DashScope search returned empty answer")

        usage = data.get("usage") or {}
        return {
            "answer": answer,
            "tool_messages": [f"DashScope 内置搜索已执行，strategy={self.search_strategy}"],
            "structured_outputs": [],
            "stages": [{"group": "search", "step": "dashscope_builtin_search"}],
            "provider": "dashscope_builtin_search",
            "model": self.model,
            "usage": usage,
        }
