from __future__ import annotations

import argparse
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

try:
    import serial
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    sys.exit(2)

DEFAULT_PROJECT = Path(r"D:\esp32\esp32idf-robot")
DEFAULT_MCP = Path(r"D:\esp32\MCP-robot")
DEFAULT_HEALTH = "http://127.0.0.1:8080/healthz"
DEFAULT_TTS_TEST = "http://127.0.0.1:8080/api/esp32/tts-test"

KEY_PATTERNS = [
    re.compile(r"websocket connected", re.I),
    re.compile(r"websocket disconnected", re.I),
    re.compile(r"tts segment", re.I),
    re.compile(r"tts audio chunk", re.I),
    re.compile(r"tts stream end", re.I),
    re.compile(r"play tts wav", re.I),
    re.compile(r"tts play finished", re.I),
    re.compile(r"wav parse failed", re.I),
    re.compile(r"base64 .*failed", re.I),
    re.compile(r"Guru Meditation|panic|abort", re.I),
]


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def http_get_json(url: str, timeout: float = 5.0) -> dict | None:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            data = response.read().decode("utf-8", errors="replace")
        return json.loads(data)
    except Exception:
        return None


def http_get_text(url: str, timeout: float = 60.0) -> str:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read().decode("utf-8", errors="replace")


def start_mcp(mcp_dir: Path) -> subprocess.Popen | None:
    script = mcp_dir / "start_mcp_robot.ps1"
    if not script.exists():
        print(f"[host] MCP start script not found: {script}")
        return None
    log_dir = mcp_dir / "data" / "debug_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    out_path = log_dir / f"mcp_started_by_tts_test_{now_stamp()}.log"
    err_path = log_dir / f"mcp_started_by_tts_test_{now_stamp()}.err.log"
    out = open(out_path, "w", encoding="utf-8", errors="replace")
    err = open(err_path, "w", encoding="utf-8", errors="replace")
    print(f"[host] starting MCP, logs: {out_path}")
    return subprocess.Popen(
        ["powershell", "-ExecutionPolicy", "Bypass", "-File", str(script)],
        cwd=str(mcp_dir),
        stdout=out,
        stderr=err,
        stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
    )


def wait_health(url: str, timeout_seconds: float) -> dict | None:
    deadline = time.time() + timeout_seconds
    last = None
    while time.time() < deadline:
        last = http_get_json(url, timeout=3.0)
        if last is not None:
            return last
        time.sleep(1.0)
    return last


def serial_reader(port: str, baud: int, log_path: Path, stop: threading.Event, lines: queue.Queue[str]) -> None:
    try:
        with serial.Serial(port=port, baudrate=baud, timeout=0.2, write_timeout=0.2) as ser, open(
            log_path, "w", encoding="utf-8", errors="replace"
        ) as log:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            print(f"[host] serial opened: {port} {baud}")
            buffer = bytearray()
            while not stop.is_set():
                data = ser.read(512)
                if not data:
                    continue
                buffer.extend(data)
                while b"\n" in buffer:
                    raw, _, buffer = buffer.partition(b"\n")
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    log.write(line + "\n")
                    log.flush()
                    lines.put(line)
            if buffer:
                line = buffer.decode("utf-8", errors="replace").rstrip("\r")
                log.write(line + "\n")
                lines.put(line)
    except Exception as exc:
        lines.put(f"[host] serial error: {exc}")


def print_matching_lines(lines: queue.Queue[str], stop: threading.Event, summary: dict[str, int]) -> None:
    while not stop.is_set() or not lines.empty():
        try:
            line = lines.get(timeout=0.2)
        except queue.Empty:
            continue
        matched = False
        for pattern in KEY_PATTERNS:
            if pattern.search(line):
                summary[pattern.pattern] = summary.get(pattern.pattern, 0) + 1
                matched = True
        if matched or line.startswith("[host]"):
            print(line)


def trigger_tts(tts_url: str, text: str) -> None:
    url = tts_url + "?" + urllib.parse.urlencode({"text": text})
    print(f"[host] triggering TTS: {text}")
    try:
        body = http_get_text(url, timeout=120.0)
        print(f"[host] tts endpoint returned: {body[:300]}")
    except Exception as exc:
        print(f"[host] tts endpoint failed: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32 MCP TTS serial integration test")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--mcp", type=Path, default=DEFAULT_MCP)
    parser.add_argument("--health-url", default=DEFAULT_HEALTH)
    parser.add_argument("--tts-url", default=DEFAULT_TTS_TEST)
    parser.add_argument("--text", default="你好，我是语音测试。")
    parser.add_argument("--startup-wait", type=float, default=35.0)
    parser.add_argument("--after-trigger", type=float, default=35.0)
    parser.add_argument("--start-mcp", action="store_true")
    args = parser.parse_args()

    log_dir = args.project / "debug_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    serial_log = log_dir / f"tts_serial_{now_stamp()}.log"
    mcp_proc = None

    health = http_get_json(args.health_url, timeout=3.0)
    if health is None and args.start_mcp:
        mcp_proc = start_mcp(args.mcp)
        health = wait_health(args.health_url, 45.0)
    print(f"[host] MCP health: {json.dumps(health, ensure_ascii=False)[:800] if health else 'not reachable'}")

    stop = threading.Event()
    lines: queue.Queue[str] = queue.Queue()
    summary: dict[str, int] = {}
    reader = threading.Thread(target=serial_reader, args=(args.port, args.baud, serial_log, stop, lines), daemon=True)
    printer = threading.Thread(target=print_matching_lines, args=(lines, stop, summary), daemon=True)
    reader.start()
    printer.start()

    try:
        print(f"[host] waiting {args.startup_wait:.1f}s for ESP32 websocket")
        time.sleep(args.startup_wait)
        trigger_tts(args.tts_url, args.text)
        print(f"[host] waiting {args.after_trigger:.1f}s after TTS trigger")
        time.sleep(args.after_trigger)
    finally:
        stop.set()
        reader.join(timeout=3.0)
        printer.join(timeout=3.0)

    print(f"[host] serial log: {serial_log}")
    print(f"[host] summary: {json.dumps(summary, ensure_ascii=False)}")
    if mcp_proc and mcp_proc.poll() is not None:
        print(f"[host] MCP process exited early: {mcp_proc.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
