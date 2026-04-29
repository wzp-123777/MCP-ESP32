from __future__ import annotations

import json
import re
from typing import Any

import httpx

from tools.base import BaseTool, ToolResult


class AmapTool(BaseTool):
    name = "maps.amap"
    description = "调用高德地图进行地理编码、逆地理编码、路径规划与轨迹纠偏。"
    output_schema = {
        "type": "object",
        "properties": {
            "formatted_address": {"type": "string"},
            "location": {"type": "string"},
            "mode": {"type": "string"},
            "origin": {"type": "string"},
            "destination": {"type": "string"},
            "distance": {"type": ["string", "number", "null"]},
            "duration": {"type": ["string", "number", "null"]},
            "steps": {"type": "array"},
            "points": {"type": "array"},
        },
    }
    risk_level = "high"
    can_direct_device = True
    tags = ("maps", "navigation", "geocode", "device_control")
    input_schema = {
        "type": "object",
        "properties": {
            "action": {"type": "string", "enum": ["geocode", "reverse_geocode", "route_plan", "trajectory_correct"]},
            "location": {"type": "string", "description": "地址或经纬度(lng,lat)"},
            "origin": {"type": "string", "description": "起点，经纬度或地址"},
            "destination": {"type": "string", "description": "终点，经纬度或地址"},
            "mode": {"type": "string", "enum": ["driving", "walking", "riding"], "default": "driving"},
            "city": {"type": "string", "description": "地理编码时可选城市"},
            "strategy": {"type": "integer", "description": "路径规划策略，仅 driving 时生效", "default": 0},
            "points": {"type": "array", "description": "轨迹纠偏点列表，每个点可传 x/y 或 lng/lat，以及可选 sp/ag/tm"},
            "polyline": {"type": "string", "description": "轨迹纠偏输入，也可传 lng,lat|lng,lat 的简化字符串"},
        },
        "required": ["action"],
    }

    def __init__(self, api_key: str, timeout_seconds: float = 20.0) -> None:
        self.api_key = api_key
        self.timeout_seconds = timeout_seconds

    def should_require_approval(self, arguments: dict[str, Any]) -> bool:
        action = str(arguments.get("action") or "").strip().lower()
        return action in {"route_plan", "trajectory_correct", "trajectory_correction", "grasp_road"}

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
            if action in {"trajectory_correct", "trajectory_correction", "grasp_road"}:
                return await self._trajectory_correct(arguments)
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
        elif mode == "riding":
            payload = await self._request(
                "https://restapi.amap.com/v4/direction/bicycling",
                {"key": self.api_key, "origin": origin, "destination": destination},
            )
            route = payload.get("data") or {}
            paths = route.get("paths") or []
            first = paths[0] if paths else {}
        else:
            payload = await self._request(
                "https://restapi.amap.com/v3/direction/driving",
                {
                    "key": self.api_key,
                    "origin": origin,
                    "destination": destination,
                    "strategy": int(arguments.get("strategy") or 0),
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

    async def _trajectory_correct(self, arguments: dict[str, Any]) -> ToolResult:
        points = self._normalize_track_points(arguments)
        if len(points) < 2:
            return ToolResult(self.name, False, None, error="trajectory_correct requires at least 2 points")
        if len(points) > 500:
            return ToolResult(self.name, False, None, error="trajectory_correct supports at most 500 points")

        url = f"https://restapi.amap.com/v4/grasproad/driving?key={self.api_key}"
        headers = {"Content-Type": "application/json"}
        async with httpx.AsyncClient(timeout=self.timeout_seconds) as client:
            response = await client.post(url, content=json.dumps(points), headers=headers)
            response.raise_for_status()
            payload = response.json()

        if int(payload.get("errcode") or 0) != 0:
            raise RuntimeError(str(payload.get("errmsg") or payload.get("errdetail") or "unknown grasp road error"))

        data = payload.get("data") or {}
        corrected = data.get("points") or []
        if isinstance(corrected, dict):
            corrected = [corrected]
        content = {
            "distance": data.get("distance"),
            "input_count": len(points),
            "corrected_count": len(corrected),
            "points": corrected,
        }
        device_payload = {
            "type": "trajectory_corrected",
            "provider": "amap",
            "result": content,
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
        if payload.get("errcode") not in (None, 0, "0"):
            info = payload.get("errmsg") or payload.get("errdetail") or "unknown amap error"
            raise RuntimeError(str(info))
        return payload

    def _normalize_track_points(self, arguments: dict[str, Any]) -> list[dict[str, Any]]:
        raw_points = arguments.get("points")
        if isinstance(raw_points, list) and raw_points:
            normalized: list[dict[str, Any]] = []
            for index, item in enumerate(raw_points):
                if not isinstance(item, dict):
                    continue
                x = item.get("x", item.get("lng"))
                y = item.get("y", item.get("lat"))
                if x is None or y is None:
                    location = str(item.get("location") or "").strip()
                    if "," in location:
                        lng, lat = [part.strip() for part in location.split(",", 1)]
                        x, y = lng, lat
                if x is None or y is None:
                    continue
                normalized.append(
                    {
                        "x": float(x),
                        "y": float(y),
                        "sp": float(item.get("sp") or item.get("speed") or 0),
                        "ag": float(item.get("ag") or item.get("direction") or 0),
                        "tm": int(item.get("tm") or item.get("timestamp") or index + 1),
                    }
                )
            return normalized

        polyline = str(arguments.get("polyline") or "").strip()
        if not polyline:
            return []
        normalized = []
        for index, pair in enumerate(re.split(r"[|;]", polyline), start=1):
            item = pair.strip()
            if not item or "," not in item:
                continue
            lng, lat = [part.strip() for part in item.split(",", 1)]
            normalized.append({"x": float(lng), "y": float(lat), "sp": 0, "ag": 0, "tm": index})
        return normalized
