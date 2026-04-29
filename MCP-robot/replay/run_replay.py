from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[1]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

from app_config import load_config
from runtime import RobotRuntime


class ReplayConnectionManager:
    def __init__(self, *, esp32_connected: bool = False) -> None:
        self.qq_messages: list[dict[str, Any]] = []
        self.esp32_messages: list[dict[str, Any]] = []
        self._esp32_connected = esp32_connected

    @property
    def is_esp32_connected(self) -> bool:
        return self._esp32_connected

    async def send_to_qq(self, message: dict[str, Any]) -> None:
        self.qq_messages.append(message)

    async def send_to_esp32(self, message: dict[str, Any]) -> None:
        self.esp32_messages.append(message)


async def main() -> int:
    parser = argparse.ArgumentParser(description="Replay MCP-Robot payload fixtures.")
    parser.add_argument("--fixture", required=True, help="JSON fixture path.")
    parser.add_argument("--wait", type=float, default=6.0, help="Seconds to wait for background tasks.")
    args = parser.parse_args()

    fixture_path = Path(args.fixture).resolve()
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    channel = str(fixture.get("channel") or "napcat").strip().lower()
    payload = fixture.get("payload") or {}

    config = load_config()
    manager = ReplayConnectionManager(esp32_connected=channel == "esp32")
    runtime = RobotRuntime(config, manager)

    if channel == "napcat":
        await runtime.handle_napcat_payload(payload)
    elif channel == "esp32":
        await runtime.handle_esp32_payload(payload)
    else:
        raise SystemExit(f"unsupported channel: {channel}")

    deadline = asyncio.get_running_loop().time() + max(0.1, args.wait)
    while asyncio.get_running_loop().time() < deadline:
        await runtime.drain_background_tasks(timeout_seconds=1.0)
        if channel == "napcat" and manager.qq_messages:
            break
        if channel == "esp32" and manager.esp32_messages:
            break
        await asyncio.sleep(0.2)
    print(
        json.dumps(
            {
                "fixture": str(fixture_path),
                "qq_messages": manager.qq_messages,
                "esp32_messages": manager.esp32_messages,
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
