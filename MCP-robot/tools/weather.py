from __future__ import annotations

from typing import Any

import httpx

from tools.base import BaseTool, ToolResult


class SeniverseWeatherTool(BaseTool):
    name = "weather.query"
    description = "查询心知天气实时天气或未来几日预报。"
    input_schema = {
        "type": "object",
        "properties": {
            "location": {"type": "string", "description": "城市名称、地名或经纬度"},
            "kind": {"type": "string", "enum": ["now", "daily"], "default": "now"},
            "days": {"type": "integer", "minimum": 1, "maximum": 15, "default": 3},
            "language": {"type": "string", "default": "zh-Hans"},
            "unit": {"type": "string", "enum": ["c", "f"], "default": "c"},
        },
        "required": ["location"],
    }

    def __init__(self, api_key: str, timeout_seconds: float = 15.0) -> None:
        self.api_key = api_key
        self.timeout_seconds = timeout_seconds

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        if not self.api_key:
            return ToolResult(self.name, False, None, error="SENIVERSE_API_KEY is missing")
        kind = (arguments.get("kind") or "now").strip().lower()
        location = (arguments.get("location") or "").strip()
        if not location:
            return ToolResult(self.name, False, None, error="location is required")
        endpoint = "now.json" if kind == "now" else "daily.json"
        params = {
            "key": self.api_key,
            "location": location,
            "language": arguments.get("language") or "zh-Hans",
            "unit": arguments.get("unit") or "c",
        }
        if kind != "now":
            params["start"] = 0
            params["days"] = min(max(int(arguments.get("days") or 3), 1), 15)
        url = f"https://api.seniverse.com/v3/weather/{endpoint}"
        try:
            async with httpx.AsyncClient(timeout=self.timeout_seconds) as client:
                response = await client.get(url, params=params)
                response.raise_for_status()
                payload = response.json()
        except Exception as exc:
            return ToolResult(self.name, False, None, error=str(exc))

        results = payload.get("results") or []
        if not results:
            return ToolResult(self.name, False, None, error="empty weather response")
        item = results[0]
        if kind == "now":
            now = item.get("now") or {}
            content = {
                "location": item.get("location", {}).get("name", location),
                "text": now.get("text"),
                "temperature": now.get("temperature"),
                "feels_like": now.get("feels_like"),
                "humidity": now.get("humidity"),
                "wind_direction": now.get("wind_direction"),
                "wind_scale": now.get("wind_scale"),
                "updated_at": item.get("last_update"),
            }
        else:
            content = {
                "location": item.get("location", {}).get("name", location),
                "daily": item.get("daily") or [],
                "updated_at": item.get("last_update"),
            }
        return ToolResult(self.name, True, content)
