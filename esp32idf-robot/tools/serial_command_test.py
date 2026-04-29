from __future__ import annotations

import argparse
import queue
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    sys.exit(2)

DEFAULT_PROJECT = Path(r"D:\esp32\esp32idf-robot")

KEY_PATTERNS = [
    re.compile(r"websocket connected", re.I),
    re.compile(r"websocket disconnected", re.I),
    re.compile(r"ASK debug|REC debug", re.I),
    re.compile(r"SET press|SET release", re.I),
    re.compile(r"send text request", re.I),
    re.compile(r"audio stream begin|audio stream end", re.I),
    re.compile(r"capture started|capture stopped", re.I),
    re.compile(r"assistant_status|assistant_done|assistant_error|tts_error", re.I),
    re.compile(r"tts segment", re.I),
    re.compile(r"tts stream end", re.I),
    re.compile(r"play tts wav", re.I),
    re.compile(r"tts play finished", re.I),
    re.compile(r"wav parse failed|base64 .*failed", re.I),
    re.compile(r"Guru Meditation|panic|abort", re.I),
]

FAIL_PATTERNS = [
    re.compile(r"wav parse failed|base64 .*failed", re.I),
    re.compile(r"assistant_error|tts_error", re.I),
    re.compile(r"Guru Meditation|panic|abort", re.I),
]


def now_stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def serial_reader(port: str, baud: int, log_path: Path, stop: threading.Event, lines: queue.Queue[str]) -> None:
    try:
        with serial.Serial(port=port, baudrate=baud, timeout=0.2, write_timeout=1.0) as ser, open(
            log_path, "w", encoding="utf-8", errors="replace"
        ) as log:
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            lines.put(f"[host] serial opened: {port} {baud}")
            setattr(serial_reader, "ser", ser)
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
    except Exception as exc:
        lines.put(f"[host] serial error: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a debug command to ESP32 and watch serial result.")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--command", required=True, help='Serial command, for example: "ASK 你好" or "REC 2500"')
    parser.add_argument("--connect-timeout", type=float, default=35.0)
    parser.add_argument("--result-timeout", type=float, default=120.0)
    args = parser.parse_args()

    log_dir = args.project / "debug_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"serial_cmd_{now_stamp()}.log"

    stop = threading.Event()
    lines: queue.Queue[str] = queue.Queue()
    thread = threading.Thread(target=serial_reader, args=(args.port, args.baud, log_path, stop, lines), daemon=True)
    thread.start()

    saw_connected = False
    saw_command = False
    saw_play_finished = False
    saw_failure = False
    command_sent = False

    try:
        connect_deadline = time.time() + args.connect_timeout
        result_deadline = time.time() + args.connect_timeout + args.result_timeout
        while time.time() < result_deadline:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                if not command_sent and (saw_connected or time.time() >= connect_deadline):
                    ser = getattr(serial_reader, "ser", None)
                    if ser is not None:
                        command = args.command.rstrip("\r\n") + "\n"
                        print(f"[host] send: {command.strip()}")
                        ser.write(command.encode("utf-8"))
                        ser.flush()
                        command_sent = True
                continue

            if any(pattern.search(line) for pattern in KEY_PATTERNS) or line.startswith("[host]"):
                print(line)
            if "websocket connected" in line.lower():
                saw_connected = True
            if "ASK debug" in line or "REC debug" in line or "SET press" in line:
                saw_command = True
            if any(pattern.search(line) for pattern in FAIL_PATTERNS):
                saw_failure = True
            if "tts play finished" in line.lower() and "ESP_OK" in line:
                saw_play_finished = True
                break

            if not command_sent and (saw_connected or time.time() >= connect_deadline):
                ser = getattr(serial_reader, "ser", None)
                if ser is not None:
                    command = args.command.rstrip("\r\n") + "\n"
                    print(f"[host] send: {command.strip()}")
                    ser.write(command.encode("utf-8"))
                    ser.flush()
                    command_sent = True
    finally:
        stop.set()
        thread.join(timeout=3.0)

    print(f"[host] serial log: {log_path}")
    print(
        f"[host] result connected={saw_connected} command={saw_command} "
        f"play_finished={saw_play_finished} failure={saw_failure}"
    )
    return 0 if saw_connected and saw_command and saw_play_finished and not saw_failure else 1


if __name__ == "__main__":
    raise SystemExit(main())
