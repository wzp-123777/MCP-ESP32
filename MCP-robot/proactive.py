from __future__ import annotations

import asyncio
import json
import logging
import re
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Awaitable, Callable

import httpx

from app_config import AppConfig
from tools import SeniverseWeatherTool


logger = logging.getLogger("MCP_Robot.Proactive")


ComposeNotice = Callable[[str, str, str], Awaitable[str]]
SendDashboard = Callable[[dict[str, Any]], Awaitable[None]]
SendNotice = Callable[[dict[str, Any], str], Awaitable[None]]
TraceEvent = Callable[[str, dict[str, Any]], None]


@dataclass(slots=True)
class CalendarEvent:
    kind: str
    title: str
    start: datetime
    end: datetime | None = None
    location: str = ""
    note: str = ""
    remind_minutes: int = 20
    enabled: bool = True


def _now_local() -> datetime:
    return datetime.now().astimezone()


_WEEKDAY_LABELS = ("周一", "周二", "周三", "周四", "周五", "周六", "周日")


def _calendar_today_label(now: datetime | None = None) -> str:
    current = now or _now_local()
    return f"{current.month:02d}月{current.day:02d}日 {_WEEKDAY_LABELS[current.weekday()]}"


def _event_start_label(start: datetime, now: datetime | None = None) -> str:
    current = now or _now_local()
    if start.date() == current.date():
        return start.strftime("今天 %H:%M")
    if start.date() == (current + timedelta(days=1)).date():
        return start.strftime("明天 %H:%M")
    return start.strftime("%m-%d %H:%M")


def _parse_datetime(value: str) -> datetime | None:
    text = str(value or "").strip()
    if not text:
        return None
    for candidate in (text, text.replace(" ", "T", 1)):
        try:
            dt = datetime.fromisoformat(candidate)
            if dt.tzinfo is None:
                dt = dt.astimezone()
            return dt.astimezone()
        except Exception:
            continue
    for fmt in ("%Y-%m-%d %H:%M", "%Y/%m/%d %H:%M"):
        try:
            return datetime.strptime(text, fmt).astimezone()
        except Exception:
            continue
    return None


def _read_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        logger.warning("read json failed: %s %s", path, exc)
        return default


def _write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def _to_int(value: Any, default: int = 0) -> int:
    try:
        return int(float(str(value)))
    except Exception:
        return default


def _safe_text(value: Any, max_len: int = 120) -> str:
    return re.sub(r"\s+", " ", str(value or "")).strip()[:max_len]


def _normalize_event_kind(value: Any) -> str:
    text = re.sub(r"[^0-9A-Za-z_\-\u4e00-\u9fff]+", "", str(value or "").strip().lower())
    if text in {"alarm", "timer", "闹钟", "倒计时"}:
        return "alarm"
    if text in {"reminder", "remind", "提醒", "提醒事项"}:
        return "reminder"
    return "calendar"


def _is_alarm_like(event: CalendarEvent) -> bool:
    return event.kind in {"alarm", "reminder"}


def _countdown_label(start: datetime, now: datetime | None = None) -> str:
    seconds_left = int((start - (now or _now_local())).total_seconds())
    if seconds_left <= 0:
        return "正在提醒"
    minutes_left = max(1, (seconds_left + 59) // 60)
    days, remainder = divmod(minutes_left, 24 * 60)
    hours, minutes = divmod(remainder, 60)
    if days > 0:
        if hours > 0:
            return f"{days}天{hours}小时"
        return f"{days}天"
    if hours > 0:
        return f"{hours}小时{minutes:02d}分钟"
    return f"{minutes}分钟"


def _redact_secret(text: Any, *secrets: str) -> str:
    result = str(text or "")
    for secret in secrets:
        if secret:
            result = result.replace(secret, "***")
    return result


class ProactiveEngine:
    def __init__(
        self,
        *,
        config: AppConfig,
        weather_tool: SeniverseWeatherTool,
        compose_notice: ComposeNotice,
        send_dashboard: SendDashboard,
        send_notice: SendNotice,
        trace: TraceEvent,
    ) -> None:
        self.config = config
        self.weather_tool = weather_tool
        self.compose_notice = compose_notice
        self.send_dashboard = send_dashboard
        self.send_notice = send_notice
        self.trace = trace
        self._lock = asyncio.Lock()
        self._last_weather_fetch = 0.0
        self._last_dashboard_send = 0.0
        self._last_notice_send = 0.0
        self._weather_now: dict[str, Any] = {}
        self._weather_daily: dict[str, Any] = {}
        self._dashboard: dict[str, Any] = self._empty_dashboard()
        self._state_file = self.config.data_dir / "proactive_state.json"

    def snapshot(self) -> dict[str, Any]:
        return json.loads(json.dumps(self._dashboard, ensure_ascii=False))

    async def run(self) -> None:
        if not self.config.proactive_enabled:
            logger.info("proactive engine disabled")
            return
        await self.poll_once(force_weather=True, force_dashboard=True)
        while True:
            try:
                await self.poll_once()
            except asyncio.CancelledError:
                raise
            except Exception:
                logger.exception("proactive poll failed")
            await asyncio.sleep(max(10, self.config.proactive_tick_seconds))

    async def poll_once(self, *, force_weather: bool = False, force_dashboard: bool = False) -> dict[str, Any]:
        async with self._lock:
            now = time.time()
            if force_weather or now - self._last_weather_fetch >= self.config.aiot_weather_scan_seconds:
                await self._refresh_weather()
                self._last_weather_fetch = now
            calendar = self._load_calendar_events()
            dashboard = self._build_dashboard(calendar)
            self._dashboard = dashboard
            due_events = self._due_events(calendar)
            for event in due_events:
                await self._maybe_send_notice(event)
            if force_dashboard or now - self._last_dashboard_send >= 60:
                await self.send_dashboard(dashboard)
                self._last_dashboard_send = now
            return dashboard

    async def _refresh_weather(self) -> None:
        location = self.config.aiot_weather_location
        if self.config.tool_api.amap_key:
            await self._refresh_weather_from_amap()
            if self._weather_now and (
                isinstance(self._weather_daily, dict) and self._weather_daily.get("daily")
            ):
                self.trace(
                    "proactive.weather.refresh",
                    {"location": location, "provider": "amap", "now_ok": True, "daily_ok": True},
                )
                return

        now_result = await self.weather_tool.execute({"location": location, "kind": "now"})
        if now_result.ok and isinstance(now_result.content, dict):
            self._weather_now = dict(now_result.content)
        else:
            logger.warning("weather now failed: %s", _redact_secret(now_result.error, self.config.tool_api.seniverse_key))
        daily_result = await self.weather_tool.execute({"location": location, "kind": "daily", "days": 3})
        if daily_result.ok and isinstance(daily_result.content, dict):
            self._weather_daily = dict(daily_result.content)
        else:
            logger.warning("weather daily failed: %s", _redact_secret(daily_result.error, self.config.tool_api.seniverse_key))
        weather_now_missing = not self._weather_now
        weather_daily_missing = not (
            isinstance(self._weather_daily, dict) and self._weather_daily.get("daily")
        )
        if (
            self.config.tool_api.amap_key
            and (weather_now_missing or weather_daily_missing or not now_result.ok or not daily_result.ok)
        ):
            await self._refresh_weather_from_amap()
        self.trace(
            "proactive.weather.refresh",
            {
                "location": location,
                "now_ok": bool(now_result.ok),
                "daily_ok": bool(daily_result.ok),
                "now_error": _redact_secret(now_result.error, self.config.tool_api.seniverse_key),
                "daily_error": _redact_secret(daily_result.error, self.config.tool_api.seniverse_key),
            },
        )

    async def _refresh_weather_from_amap(self) -> None:
        adcode = self.config.aiot_weather_adcode
        params_base = {"key": self.config.tool_api.amap_key, "city": adcode}
        try:
            async with httpx.AsyncClient(timeout=12.0) as client:
                live_resp = await client.get(
                    "https://restapi.amap.com/v3/weather/weatherInfo",
                    params={**params_base, "extensions": "base", "output": "JSON"},
                )
                live_resp.raise_for_status()
                live_payload = live_resp.json()
                lives = live_payload.get("lives") or []
                if lives:
                    live = lives[0]
                    self._weather_now = {
                        "location": live.get("city") or self.config.aiot_weather_location,
                        "text": live.get("weather"),
                        "temperature": live.get("temperature"),
                        "humidity": live.get("humidity"),
                        "wind_direction": live.get("winddirection"),
                        "wind_scale": live.get("windpower"),
                        "updated_at": live.get("reporttime"),
                    }
                daily_resp = await client.get(
                    "https://restapi.amap.com/v3/weather/weatherInfo",
                    params={**params_base, "extensions": "all", "output": "JSON"},
                )
                daily_resp.raise_for_status()
                daily_payload = daily_resp.json()
                forecasts = daily_payload.get("forecasts") or []
                if forecasts:
                    casts = forecasts[0].get("casts") or []
                    self._weather_daily = {
                        "location": forecasts[0].get("city") or self.config.aiot_weather_location,
                        "daily": [
                            {
                                "date": item.get("date"),
                                "text_day": item.get("dayweather"),
                                "text_night": item.get("nightweather"),
                                "high": item.get("daytemp"),
                                "low": item.get("nighttemp"),
                            }
                            for item in casts[:3]
                        ],
                        "updated_at": self._weather_now.get("updated_at") or "",
                    }
            self.trace("proactive.weather.amap_refresh", {"adcode": adcode, "ok": bool(self._weather_now)})
        except Exception as exc:
            logger.warning("amap weather failed: %s", _redact_secret(exc, self.config.tool_api.amap_key))

    def _load_calendar_events(self) -> list[CalendarEvent]:
        path = self.config.aiot_calendar_file
        if not path.exists():
            _write_json(path, [])
        raw = _read_json(path, [])
        events: list[CalendarEvent] = []
        for item in raw if isinstance(raw, list) else []:
            if not isinstance(item, dict):
                continue
            start = _parse_datetime(str(item.get("start") or ""))
            if start is None:
                continue
            end = _parse_datetime(str(item.get("end") or "")) if item.get("end") else None
            title = _safe_text(item.get("title"), 60)
            if not title:
                continue
            kind = _normalize_event_kind(item.get("kind") or item.get("type"))
            raw_remind_minutes = _to_int(item.get("remind_minutes"), 20)
            events.append(
                CalendarEvent(
                    kind=kind,
                    title=title,
                    start=start,
                    end=end,
                    location=_safe_text(item.get("location"), 40),
                    note=_safe_text(item.get("note"), 80),
                    remind_minutes=max(0 if kind in {"alarm", "reminder"} else 1, min(1440, raw_remind_minutes)),
                    enabled=bool(item.get("enabled", True)),
                )
            )
        events.sort(key=lambda item: item.start)
        return events

    def _build_dashboard(self, calendar: list[CalendarEvent]) -> dict[str, Any]:
        now_weather = self._weather_now or {}
        daily = self._weather_daily.get("daily") if isinstance(self._weather_daily, dict) else []
        first_daily = daily[0] if isinstance(daily, list) and daily else {}
        location = _safe_text(now_weather.get("location") or self.config.aiot_weather_location, 24)
        weather_text = _safe_text(now_weather.get("text") or first_daily.get("text_day") or "天气待更新", 24)
        temp = _safe_text(now_weather.get("temperature"), 8)
        humidity = _safe_text(now_weather.get("humidity"), 8)
        high = _safe_text(first_daily.get("high"), 8)
        low = _safe_text(first_daily.get("low"), 8)
        weather_title = f"{location} {weather_text}"
        if temp:
            weather_title = f"{weather_title} {temp}C"
        weather_detail = " / ".join(
            item
            for item in (
                f"湿度{humidity}%" if humidity else "",
                f"{low}-{high}C" if high or low else "",
                _safe_text(now_weather.get("wind_direction"), 8),
            )
            if item
        ) or "等待天气数据"
        weather_alert = self._weather_alert_text()

        now_local = _now_local()
        calendar_title = _calendar_today_label(now_local)
        next_alarm = self._next_alarm_event(calendar)
        next_event = self._next_calendar_event(calendar)
        previous_reminder = str(self._dashboard.get("reminder_text") or "").strip()
        reminder_text = previous_reminder
        if next_alarm:
            start_label = _event_start_label(next_alarm.start, now_local)
            countdown = _countdown_label(next_alarm.start, now_local)
            calendar_detail = f"闹钟 {start_label} {next_alarm.title}"
            reminder_text = f"闹钟倒计时 {countdown}：{next_alarm.title}"
        elif next_event:
            start_label = _event_start_label(next_event.start, now_local)
            detail_extra = next_event.location or next_event.note or f"提前{next_event.remind_minutes}分钟提醒"
            calendar_detail = f"下个日程 {start_label} {next_event.title}"
            if detail_extra:
                calendar_detail = f"{calendar_detail} / {detail_extra}"
        else:
            calendar_detail = "今天暂无重要日程"

        return {
            "type": "dashboard_update",
            "source": "aiot_proactive",
            "location": location,
            "weather_title": weather_title[:64],
            "weather_detail": weather_detail[:96],
            "weather_alert": weather_alert[:96],
            "calendar_title": calendar_title[:80],
            "calendar_detail": calendar_detail[:96],
            "reminder_text": reminder_text[:96],
            "updated_at": _now_local().strftime("%H:%M"),
        }

    def _weather_alert_text(self) -> str:
        if not self._weather_now and not (self._weather_daily.get("daily") if isinstance(self._weather_daily, dict) else None):
            return ""
        daily = self._weather_daily.get("daily") if isinstance(self._weather_daily, dict) else []
        today = daily[0] if isinstance(daily, list) and daily and isinstance(daily[0], dict) else {}
        text = " ".join(
            str(item or "")
            for item in (
                self._weather_now.get("text"),
                today.get("text_day"),
                today.get("text_night"),
            )
        )
        temp = _to_int(self._weather_now.get("temperature"), -1000)
        if any(token in text for token in ("雨", "雪", "雷", "阵雨")):
            return "可能有雨雪，出门留意雨具。"
        if temp >= 32:
            return "天气偏热，注意补水。"
        if temp <= 3:
            return "天气很冷，出门多穿一点。"
        return "天气正常。"

    def _next_calendar_event(self, calendar: list[CalendarEvent]) -> CalendarEvent | None:
        now = _now_local() - timedelta(minutes=5)
        for event in calendar:
            if event.enabled and event.start >= now:
                return event
        return None

    def _next_alarm_event(self, calendar: list[CalendarEvent]) -> CalendarEvent | None:
        now = _now_local() - timedelta(minutes=2)
        for event in calendar:
            if event.enabled and _is_alarm_like(event) and event.start >= now:
                return event
        return None

    def _due_events(self, calendar: list[CalendarEvent]) -> list[dict[str, Any]]:
        due: list[dict[str, Any]] = []
        now = _now_local()
        weather_key = f"weather:{now.date().isoformat()}:{self._weather_alert_text()}"
        alert = self._weather_alert_text()
        if alert and alert != "天气正常。":
            due.append(
                {
                    "kind": "weather",
                    "dedupe_key": weather_key,
                    "event_text": f"{self.config.aiot_weather_location}，{alert}",
                    "priority": "normal",
                    "fallback": alert.replace("可能", "今天"),
                }
            )
        for event in calendar:
            if not event.enabled:
                continue
            minutes_left = (event.start - now).total_seconds() / 60.0
            if -2 <= minutes_left <= event.remind_minutes:
                kind = event.kind if _is_alarm_like(event) else "calendar"
                event_prefix = "闹钟" if kind == "alarm" else ("提醒" if kind == "reminder" else "日程")
                due.append(
                    {
                        "kind": kind,
                        "dedupe_key": f"{kind}:{event.start.isoformat()}:{event.title}",
                        "title": event.title,
                        "event_text": f"{event_prefix}{event.title}，{max(0, round(minutes_left))}分钟后开始。{event.location or event.note}",
                        "priority": "high",
                        "fallback": f"{event.title}时间到了。",
                        "alarm_music_index": 2 if _is_alarm_like(event) else None,
                    }
                )
        return due

    async def _maybe_send_notice(self, event: dict[str, Any]) -> None:
        state = _read_json(self._state_file, {})
        spoken = state.get("spoken") if isinstance(state, dict) else {}
        if not isinstance(spoken, dict):
            spoken = {}
        key = str(event.get("dedupe_key") or "")
        if not key or key in spoken:
            return
        now = time.time()
        if now - self._last_notice_send < self.config.proactive_min_gap_seconds and event.get("priority") != "high":
            return
        event_text = str(event.get("event_text") or "")
        try:
            notice = await self.compose_notice(str(event.get("kind") or "aiot"), event_text, self.snapshot().get("weather_title", ""))
        except Exception as exc:
            logger.warning("compose proactive notice failed: %s", exc)
            notice = str(event.get("fallback") or event_text)[:24]
        self._dashboard["reminder_text"] = notice
        await self.send_notice(event, notice)
        spoken[key] = {"at": _now_local().isoformat(), "notice": notice}
        state = {"spoken": dict(list(spoken.items())[-200:])}
        _write_json(self._state_file, state)
        self._last_notice_send = now
        self.trace("proactive.notice.sent", {"notice_event": event, "notice": notice})

    @staticmethod
    def _empty_dashboard() -> dict[str, Any]:
        return {
            "type": "dashboard_update",
            "source": "aiot_proactive",
            "location": "",
            "weather_title": "天津东丽 天气待更新",
            "weather_detail": "等待服务器天气扫描",
            "weather_alert": "",
            "calendar_title": _calendar_today_label(),
            "calendar_detail": "今天暂无重要日程",
            "reminder_text": "",
            "updated_at": "",
        }
