from __future__ import annotations

import argparse
import json
import queue
import sys
import threading
import time
from pathlib import Path
from typing import Any


protocol_out = sys.__stdout__ or sys.stdout
protocol_lock = threading.Lock()

if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
if hasattr(protocol_out, "reconfigure"):
    protocol_out.reconfigure(encoding="utf-8", errors="replace")

# Keep GenericAgent's own prints away from the JSON protocol stream.
sys.stdout = sys.stderr


def emit(payload: dict[str, Any]) -> None:
    line = json.dumps(payload, ensure_ascii=False)
    with protocol_lock:
        protocol_out.write(line + "\n")
        protocol_out.flush()


def watch_task(request_id: str, display_queue: queue.Queue[Any], timeout_seconds: int) -> None:
    deadline = time.monotonic() + max(1, timeout_seconds)
    try:
        while True:
            remaining = max(0.1, deadline - time.monotonic())
            item = display_queue.get(timeout=remaining)
            if "done" in item:
                emit(
                    {
                        "type": "result",
                        "request_id": request_id,
                        "ok": True,
                        "result": str(item["done"]),
                    }
                )
                return
    except queue.Empty:
        emit(
            {
                "type": "result",
                "request_id": request_id,
                "ok": False,
                "error": f"GenericAgent task timeout after {timeout_seconds}s",
            }
        )
    except Exception as exc:
        emit(
            {
                "type": "result",
                "request_id": request_id,
                "ok": False,
                "error": str(exc),
            }
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent-root", required=True)
    args = parser.parse_args()

    agent_root = Path(args.agent_root).resolve()
    if str(agent_root) not in sys.path:
        sys.path.insert(0, str(agent_root))

    from agentmain import GeneraticAgent  # type: ignore

    # 配置文件路径和读写函数
    CONFIG_FILE = agent_root / "model_config.json"

    def load_model_config() -> dict:
        if CONFIG_FILE.exists():
            try:
                return json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            except Exception:
                pass
        return {}

    def _normalize_model_token(value: Any) -> str:
        token = str(value or "").strip().lower()
        if "/" in token:
            token = token.split("/", 1)[1].strip()
        return token

    def _resolve_saved_llm_index(agent: GeneraticAgent, config: dict[str, Any]) -> int | None:
        saved_index = config.get("llm_index")
        if isinstance(saved_index, int) and 0 <= saved_index < len(agent.llmclients):
            return saved_index

        for key in ("backend_name", "model_name", "display_name", "model"):
            token = _normalize_model_token(config.get(key))
            if not token:
                continue
            idx = agent._find_preferred_llm_index((token,))
            if idx is not None:
                return idx
        return None

    def save_model_config(agent: GeneraticAgent) -> None:
        backend = getattr(agent.llmclient, "backend", None)
        model_name = str(getattr(backend, "default_model", "") or getattr(backend, "model", "")).strip()
        payload = {
            "llm_index": int(agent.llm_no),
            "backend_name": str(getattr(backend, "name", "")).strip(),
            "model_name": model_name,
            "display_name": agent.get_llm_name(),
        }
        CONFIG_FILE.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

    def start_agent(preferred_keyword: str | None = None, preferred_index: int | None = None) -> GeneraticAgent:
        new_agent = GeneraticAgent()
        new_agent.verbose = False
        if preferred_index is not None:
            new_agent.next_llm(preferred_index)
        elif preferred_keyword:
            idx = new_agent._find_preferred_llm_index((preferred_keyword.lower(),))
            if idx is None:
                raise ValueError(f"找不到匹配的模型: {preferred_keyword}")
            new_agent.next_llm(idx)
        else:
            # 从配置文件读取持久化的模型
            config = load_model_config()
            idx = _resolve_saved_llm_index(new_agent, config)
            if idx is not None:
                new_agent.next_llm(idx)
        threading.Thread(target=new_agent.run, daemon=True).start()
        return new_agent

    agent = start_agent()
    emit({"type": "ready"})

    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError as exc:
            emit({"type": "error", "error": f"bad json: {exc}"})
            continue

        message_type = str(payload.get("type") or "")
        if message_type == "ping":
            emit({"type": "pong"})
            continue
        if message_type == "shutdown":
            emit({"type": "bye"})
            return 0

        if message_type == "change_model":
            request_id = str(payload.get("request_id") or "")
            command = str(payload.get("command") or "").strip()
            if not request_id:
                emit({"type": "error", "error": "missing request_id"})
                continue
            try:
                old_name = agent.get_llm_name()
                agent.abort()
                if command.isdigit():
                    next_agent = start_agent(preferred_index=int(command))
                else:
                    next_agent = start_agent(preferred_keyword=command)
                agent = next_agent
                save_model_config(agent)
                emit(
                    {
                        "type": "result",
                        "request_id": request_id,
                        "ok": True,
                        "result": f"已重建 GA 执行器，并把模型从 {old_name} 切到 {agent.get_llm_name()}",
                    }
                )
            except Exception as e:
                emit({"type": "result", "request_id": request_id, "ok": False, "error": str(e)})
            continue

        if message_type != "root_command":
            emit({"type": "error", "error": f"unsupported message type: {message_type}"})
            continue

        request_id = str(payload.get("request_id") or "")
        prompt = str(payload.get("prompt") or "").strip()
        timeout_seconds = int(payload.get("timeout") or 900)
        if not request_id:
            emit({"type": "error", "error": "missing request_id"})
            continue
        if not prompt:
            emit(
                {
                    "type": "result",
                    "request_id": request_id,
                    "ok": False,
                    "error": "empty prompt",
                }
            )
            continue

        display_queue: queue.Queue[Any] = agent.put_task(prompt, "napcat-root")
        threading.Thread(
            target=watch_task,
            args=(request_id, display_queue, timeout_seconds),
            daemon=True,
        ).start()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
