from __future__ import annotations

from typing import Any

import httpx

from tools.base import BaseTool, ToolResult


class AmapTool(BaseTool):
    name = "maps.amap"
    description = "调用高德地图进行地理编码、逆地理编码与导航路线规划。"
    input_schema = {
        "type": "object",
        "properties": {
            "action": {"type": "string", "enum": ["geocode", "reverse_geocode", "route_plan"]},
            "location": {"type": "string", "description": "地址或经纬度(lng,lat)"},
            "origin": {"type": "string", "description": "起点，经纬度或地址"},
            "destination": {"type": "string", "description": "终点，经纬度或地址"},
            "mode": {"type": "string", "enum": ["driving", "walking"], "default": "driving"},
            "city": {"type": "string", "description": "地理编码时可选城市"},
        },
        "required": ["action"],
    }

    def __init__(self, api_key: str, timeout_seconds: float = 20.0) -> None:
        self.api_key = api_key
        self.timeout_seconds = timeout_seconds

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        if not self.api_key:
            return ToolResult(self.name, False, None, error="AMAP_API_KEY is missing")
        action = (arguments.get("action") or "").strip().lower()
        try:
            if action == "geocode":
                return await self._geocode(arguments)
            if action == "reverse_geocode":
                return await self._reverse_geocode(arguments)
            if action == "route_plan":
                return await self._route_plan(arguments)
            return ToolResult(self.name, False, None, error=f"unsupported action: {action}")
        except Exception as exc:
            return ToolResult(self.name, False, None, error=str(exc))

    async def _geocode(self, arguments: dict[str, Any]) -> ToolResult:
        address = (arguments.get("location") or "").strip()
        if not address:
            return ToolResult(self.name, False, None, error="location is required for geocode")
        payload = await self._request(
            "https://restapi.amap.com/v3/geocode/geo",
            {"key": self.api_key, "address": address, "city": arguments.get("city") or ""},
        )
        geocodes = payload.get("geocodes") or []
        if not geocodes:
            return ToolResult(self.name, False, None, error="empty geocode response")
        item = geocodes[0]
        content = {
            "formatted_address": item.get("formatted_address", address),
            "location": item.get("location"),
            "level": item.get("level"),
        }
        return ToolResult(self.name, True, content)

    async def _reverse_geocode(self, arguments: dict[str, Any]) -> ToolResult:
        location = (arguments.get("location") or "").strip()
        if not location:
            return ToolResult(self.name, False, None, error="location is required for reverse_geocode")
        payload = await self._request(
            "https://restapi.amap.com/v3/geocode/regeo",
            {"key": self.api_key, "location": location, "extensions": "base"},
        )
        regeocode = payload.get("regeocode") or {}
        if not regeocode:
            return ToolResult(self.name, False, None, error="empty reverse geocode response")
        content = {
            "formatted_address": regeocode.get("formatted_address"),
            "address_component": regeocode.get("addressComponent"),
        }
        return ToolResult(self.name, True, content)

    async def _route_plan(self, arguments: dict[str, Any]) -> ToolResult:
        origin = await self._resolve_coordinate(arguments.get("origin"), arguments.get("city"))
        destination = await self._resolve_coordinate(arguments.get("destination"), arguments.get("city"))
        if not origin or not destination:
            return ToolResult(self.name, False, None, error="origin and destination are required for route_plan")
        mode = (arguments.get("mode") or "driving").strip().lower()
        if mode == "walking":
            payload = await self._request(
                "https://restapi.amap.com/v3/direction/walking",
                {"key": self.api_key, "origin": origin, "destination": destination},
            )
            route = payload.get("route") or {}
            paths = route.get("paths") or []
            first = paths[0] if paths else {}
        else:
            payload = await self._request(
                "https://restapi.amap.com/v3/direction/driving",
                {
                    "key": self.api_key,
                    "origin": origin,
                    "destination": destination,
                    "strategy": 0,
                    "extensions": "base",
                },
            )
            route = payload.get("route") or {}
            paths = route.get("paths") or []
            first = paths[0] if paths else {}
        content = {
            "mode": mode,
            "origin": origin,
            "destination": destination,
            "distance": first.get("distance"),
            "duration": first.get("duration"),
            "steps": first.get("steps") or [],
            "taxi_cost": route.get("taxi_cost"),
        }
        device_payload = {
            "type": "navigation_route",
            "provider": "amap",
            "mode": mode,
            "origin": origin,
            "destination": destination,
            "route": content,
        }
        return ToolResult(self.name, True, content, device_payload=device_payload)

    async def _resolve_coordinate(self, raw: Any, city: str | None) -> str:
        if raw is None:
            return ""
        value = str(raw).strip()
        if "," in value and all(part.strip().replace(".", "", 1).replace("-", "", 1).isdigit() for part in value.split(",", 1)):
            return value
        geocode = await self._geocode({"location": value, "city": city or ""})
        if geocode.ok and isinstance(geocode.content, dict):
            return geocode.content.get("location") or ""
        return ""

    async def _request(self, url: str, params: dict[str, Any]) -> dict[str, Any]:
        async with httpx.AsyncClient(timeout=self.timeout_seconds) as client:
            response = await client.get(url, params=params)
            response.raise_for_status()
            payload = response.json()
        if payload.get("status") not in (None, "1"):
            info = payload.get("info") or payload.get("infocode") or "unknown amap error"
            raise RuntimeError(str(info))
        return payload
