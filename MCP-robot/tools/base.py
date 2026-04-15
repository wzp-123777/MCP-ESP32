from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(slots=True)
class ToolResult:
    tool_name: str
    ok: bool
    content: Any
    device_payload: dict[str, Any] | None = None
    error: str | None = None


class BaseTool:
    name: str = ""
    description: str = ""
    input_schema: dict[str, Any] = {}

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        raise NotImplementedError

    def as_catalog_entry(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "description": self.description,
            "input_schema": self.input_schema,
        }


@dataclass(slots=True)
class ToolExecutionBundle:
    results: list[ToolResult] = field(default_factory=list)
    device_payloads: list[dict[str, Any]] = field(default_factory=list)

    def to_prompt_block(self) -> str:
        if not self.results:
            return ""
        lines = ["工具调用结果："]
        for result in self.results:
            if result.ok:
                lines.append(f"- {result.tool_name}: {result.content}")
            else:
                lines.append(f"- {result.tool_name}: ERROR {result.error}")
        return "\n".join(lines)


class ToolRegistry:
    def __init__(self) -> None:
        self._tools: dict[str, BaseTool] = {}

    def register(self, tool: BaseTool) -> None:
        if not tool.name:
            raise ValueError("tool.name cannot be empty")
        self._tools[tool.name] = tool

    def catalog(self) -> list[dict[str, Any]]:
        return [tool.as_catalog_entry() for tool in self._tools.values()]

    async def execute_plan(self, calls: list[dict[str, Any]]) -> ToolExecutionBundle:
        bundle = ToolExecutionBundle()
        for call in calls:
            name = call.get("name", "")
            arguments = call.get("arguments") or {}
            tool = self._tools.get(name)
            if tool is None:
                bundle.results.append(
                    ToolResult(
                        tool_name=name or "<missing>",
                        ok=False,
                        content=None,
                        error="unknown tool",
                    )
                )
                continue
            result = await tool.execute(arguments)
            bundle.results.append(result)
            if result.device_payload is not None:
                bundle.device_payloads.append(result.device_payload)
        return bundle
