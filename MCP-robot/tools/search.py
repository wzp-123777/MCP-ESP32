from __future__ import annotations

from typing import Any

import httpx

from tools.base import BaseTool, ToolResult


class TavilySearchTool(BaseTool):
    name = "search.web"
    description = "使用 Tavily 进行联网搜索并返回摘要和关键结果。"
    input_schema = {
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "要搜索的问题或关键词"},
            "search_depth": {"type": "string", "enum": ["basic", "advanced"], "default": "basic"},
            "max_results": {"type": "integer", "minimum": 1, "maximum": 10, "default": 5},
            "include_answer": {"type": "boolean", "default": True},
        },
        "required": ["query"],
    }

    def __init__(self, api_key: str, timeout_seconds: float = 20.0) -> None:
        self.api_key = api_key
        self.timeout_seconds = timeout_seconds

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        if not self.api_key:
            return ToolResult(self.name, False, None, error="TAVILY_API_KEY is missing")
        query = (arguments.get("query") or "").strip()
        if not query:
            return ToolResult(self.name, False, None, error="query is required")
        payload = {
            "api_key": self.api_key,
            "query": query,
            "search_depth": arguments.get("search_depth") or "basic",
            "max_results": min(max(int(arguments.get("max_results") or 5), 1), 10),
            "include_answer": bool(arguments.get("include_answer", True)),
            "include_raw_content": False,
        }
        try:
            async with httpx.AsyncClient(timeout=self.timeout_seconds) as client:
                response = await client.post("https://api.tavily.com/search", json=payload)
                response.raise_for_status()
                result = response.json()
        except Exception as exc:
            return ToolResult(self.name, False, None, error=str(exc))

        content = {
            "answer": result.get("answer"),
            "results": [
                {
                    "title": item.get("title"),
                    "url": item.get("url"),
                    "content": item.get("content"),
                }
                for item in (result.get("results") or [])
            ],
        }
        return ToolResult(self.name, True, content)
