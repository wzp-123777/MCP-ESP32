from __future__ import annotations

import asyncio
import json
import logging
import os
import re
import uuid
from pathlib import Path
from typing import Any


logger = logging.getLogger("MCP_Robot.GenericAgent")


class GenericAgentBridge:
    def __init__(self, agent_root: Path, agent_python: Path) -> None:
        self.agent_root = agent_root
        self.agent_python = agent_python
        self.runner_script = Path(__file__).resolve().parent / "generic_agent_runner.py"
        self.daemon_script = Path(__file__).resolve().parent / "generic_agent_daemon.py"
        self.config_watch_files = [
            self.agent_root / "mykey.py",
            self.agent_root / "agentmain.py",
        ]
        self._process: asyncio.subprocess.Process | None = None
        self._stdout_task: asyncio.Task[Any] | None = None
        self._stderr_task: asyncio.Task[Any] | None = None
        self._pending: dict[str, asyncio.Future[str]] = {}
        self._ready_waiter: asyncio.Future[None] | None = None
        self._write_lock = asyncio.Lock()
        self._start_lock = asyncio.Lock()
        self._restart_count = 0
        self._worker_signature: tuple[tuple[str, int], ...] = ()

    async def run_change_model_command(self, command: str, *, timeout_seconds: int = 15) -> str:
        async def _attempt() -> str:
            await self._ensure_worker()
            request_id = uuid.uuid4().hex
            loop = asyncio.get_running_loop()
            future: asyncio.Future[str] = loop.create_future()
            self._pending[request_id] = future
            payload = {
                "type": "change_model",
                "request_id": request_id,
                "command": command.strip(),
            }
            try:
                await self._send_payload(payload)
                return await asyncio.wait_for(future, timeout=timeout_seconds)
            finally:
                self._pending.pop(request_id, None)

        try:
            return await _attempt()
        except Exception as exc:
            return f"通知 GenericAgent 修改模型失败，可能是进程严重崩溃: {exc}"

    async def run_root_command(self, command: str, *, timeout_seconds: int = 900) -> str:
        async def _attempt() -> str:
            await self._ensure_worker()
            request_id = uuid.uuid4().hex
            loop = asyncio.get_running_loop()
            future: asyncio.Future[str] = loop.create_future()
            self._pending[request_id] = future
            logger.info(
                "外部执行助手已接到任务 | 请求=%s | 排队=%s | 指令=%s",
                request_id[:8],
                len(self._pending),
                self._preview(command),
            )
            payload = {
                "type": "root_command",
                "request_id": request_id,
                "prompt": command.strip(),
                "timeout": int(timeout_seconds),
            }
            try:
                await self._send_payload(payload)
                return await asyncio.wait_for(future, timeout=timeout_seconds + 15)
            finally:
                self._pending.pop(request_id, None)

        try:
            return await _attempt()
        except Exception as exc:
            self._restart_count += 1
            logger.warning(
                "外部执行助手因请求失败准备重启 | 第%s次 | 排队=%s | 错误=%s",
                self._restart_count,
                len(self._pending),
                exc,
            )
            await self._reset_worker()
            return await _attempt()

    async def _ensure_worker(self) -> None:
        async with self._start_lock:
            current_signature = self._current_worker_signature()
            if self._process and self._process.returncode is None:
                if self._worker_signature and current_signature != self._worker_signature:
                    logger.warning(
                        "外部执行助手配置已变化，准备重启 | pid=%s | 排队=%s",
                        self._process.pid,
                        len(self._pending),
                    )
                    await self._reset_worker()
                else:
                    logger.info("外部执行助手继续复用现有进程 | pid=%s | 排队=%s", self._process.pid, len(self._pending))
                    return

            env = os.environ.copy()
            env["PYTHONIOENCODING"] = "utf-8"
            env["PYTHONUTF8"] = "1"
            self._ready_waiter = asyncio.get_running_loop().create_future()
            logger.info(
                "外部执行助手启动中 | Python=%s | 根目录=%s | 排队=%s",
                self.agent_python,
                self.agent_root,
                len(self._pending),
            )
            self._process = await asyncio.create_subprocess_exec(
                str(self.agent_python),
                "-X",
                "utf8",
                str(self.daemon_script),
                "--agent-root",
                str(self.agent_root),
                cwd=str(self.agent_root),
                env=env,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            self._stdout_task = asyncio.create_task(self._stdout_loop())
            self._stderr_task = asyncio.create_task(self._stderr_loop())
            await asyncio.wait_for(self._ready_waiter, timeout=45)
            self._worker_signature = current_signature
            logger.info("外部执行助手已就绪 | pid=%s | 排队=%s", self._process.pid if self._process else None, len(self._pending))

    async def _send_payload(self, payload: dict[str, Any]) -> None:
        if self._process is None or self._process.stdin is None or self._process.returncode is not None:
            raise RuntimeError("GenericAgent worker is not running")
        data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
        async with self._write_lock:
            self._process.stdin.write(data)
            await self._process.stdin.drain()

    async def _stdout_loop(self) -> None:
        assert self._process is not None and self._process.stdout is not None
        try:
            while True:
                line = await self._process.stdout.readline()
                if not line:
                    break
                text = self._decode_output(line).strip()
                if not text:
                    continue
                try:
                    payload = json.loads(text)
                except json.JSONDecodeError:
                    logger.warning("外部执行助手输出了无法解析的文本: %s", text)
                    continue
                await self._handle_worker_payload(payload)
        finally:
            await self._handle_worker_exit()

    async def _stderr_loop(self) -> None:
        assert self._process is not None and self._process.stderr is not None
        while True:
            line = await self._process.stderr.readline()
            if not line:
                return
            text = self._decode_output(line).rstrip()
            if text:
                logger.info("外部执行助手输出: %s", text)

    async def _handle_worker_payload(self, payload: dict[str, Any]) -> None:
        message_type = str(payload.get("type") or "")
        if message_type == "ready":
            if self._ready_waiter and not self._ready_waiter.done():
                self._ready_waiter.set_result(None)
            return

        if message_type != "result":
            logger.info("外部执行助手返回额外消息: %s", payload)
            return

        request_id = str(payload.get("request_id") or "")
        future = self._pending.get(request_id)
        if future is None or future.done():
            return
        if payload.get("ok", True):
            result = str(payload.get("result") or "")
            logger.info(
                "外部执行助手任务完成 | 请求=%s | 剩余排队=%s",
                request_id[:8],
                max(0, len(self._pending) - 1),
            )
            future.set_result(self._extract_user_facing_output(result))
        else:
            logger.warning(
                "外部执行助手任务失败 | 请求=%s | 剩余排队=%s | 错误=%s",
                request_id[:8],
                max(0, len(self._pending) - 1),
                payload.get("error"),
            )
            future.set_exception(RuntimeError(str(payload.get("error") or "GenericAgent worker returned an error")))

    async def _handle_worker_exit(self) -> None:
        process = self._process
        if process is not None:
            await process.wait()
            logger.warning(
                "外部执行助手意外退出 | pid=%s | code=%s | 排队=%s",
                process.pid,
                process.returncode,
                len(self._pending),
            )
        error = RuntimeError("GenericAgent worker stopped unexpectedly")
        if self._ready_waiter and not self._ready_waiter.done():
            self._ready_waiter.set_exception(error)
        for future in list(self._pending.values()):
            if not future.done():
                future.set_exception(error)
        self._process = None
        self._ready_waiter = None
        self._worker_signature = ()

    async def _reset_worker(self) -> None:
        process = self._process
        self._process = None
        if process and process.returncode is None:
            logger.warning("收到外部执行助手重置请求 | pid=%s | 排队=%s", process.pid, len(self._pending))
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), timeout=5)
            except asyncio.TimeoutError:
                logger.warning("外部执行助手等待退出超时，已强制结束 | pid=%s", process.pid)
                process.kill()
                await process.wait()
        for task in (self._stdout_task, self._stderr_task):
            if task is not None:
                task.cancel()
        self._stdout_task = None
        self._stderr_task = None
        if self._ready_waiter and not self._ready_waiter.done():
            self._ready_waiter.cancel()
        self._ready_waiter = None
        for future in list(self._pending.values()):
            if not future.done():
                future.cancel()
        self._pending.clear()
        self._worker_signature = ()
        logger.info("外部执行助手重置完成 | 排队=0")

    def _preview(self, text: str, limit: int = 80) -> str:
        normalized = re.sub(r"\s+", " ", (text or "").strip())
        if len(normalized) <= limit:
            return normalized
        return normalized[: limit - 3] + "..."

    def _current_worker_signature(self) -> tuple[tuple[str, int], ...]:
        signature: list[tuple[str, int]] = []
        for path in self.config_watch_files:
            try:
                stat = path.stat()
                signature.append((str(path), stat.st_mtime_ns))
            except FileNotFoundError:
                signature.append((str(path), -1))
        return tuple(signature)

    def _decode_output(self, data: bytes) -> str:
        for encoding in ("utf-8", "utf-8-sig", "gb18030", "gbk"):
            try:
                return data.decode(encoding)
            except UnicodeDecodeError:
                continue
        return data.decode("utf-8", errors="replace")

    def _extract_user_facing_output(self, raw: str) -> str:
        text = raw.replace("\r\n", "\n").strip()
        tail = self._extract_last_turn_answer(text)
        if tail:
            return tail
        text = self._strip_noise(text)
        final = self._pick_final_answer_block(text)
        if final:
            return final
        if not text:
            return "root agent 已执行，但没有返回可展示内容。"
        return text

    def _extract_last_turn_answer(self, text: str) -> str:
        matches = list(re.finditer(r"LLM Running \(Turn \d+\) \.\.\.", text))
        if not matches:
            return ""
        tail = text[matches[-1].end() :].strip()
        tail = self._strip_noise(tail)
        if tail and not self._is_noise_block(tail):
            return tail
        return ""

    def _strip_noise(self, text: str) -> str:
        text = re.sub(r"^Full prompt length:.*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\[Debug\].*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\[Cache\].*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\*\*LLM Running.*?\*\*$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^LLM Running.*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^🛠️.*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\[Action\].*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\[Status\].*?$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^code run output:\s*$", "", text, flags=re.MULTILINE)
        text = re.sub(r"<thinking>[\s\S]*?</thinking>", "", text, flags=re.IGNORECASE)
        text = re.sub(r"<summary>[\s\S]*?</summary>", "", text, flags=re.IGNORECASE)
        text = re.sub(r"`{3,}[\s\S]*?`{3,}", "", text)
        text = re.sub(r"^```[a-zA-Z_]+\s*$", "", text, flags=re.MULTILINE)
        text = re.sub(r"^\[Info\]\s*Final response to user\.\s*$", "", text, flags=re.MULTILINE)
        text = re.sub(r"<tool_use>[\s\S]*?(?:</tool_use>|$)", "", text, flags=re.IGNORECASE)
        text = re.sub(r"^</?tool[^>]*>.*?$", "", text, flags=re.IGNORECASE | re.MULTILINE)
        text = re.sub(r"<tool[\s\S]*$", "", text, flags=re.IGNORECASE)
        text = re.sub(r"\n{3,}", "\n\n", text).strip()
        return text

    def _pick_final_answer_block(self, text: str) -> str:
        blocks = [block.strip() for block in re.split(r"\n\s*\n", text) if block.strip()]
        if not blocks:
            return ""
        noise_prefixes = (
            "Id ProcessName",
            "-- -----------",
            "=== ",
        )
        useful: list[str] = []
        for block in blocks:
            first = block.splitlines()[0].strip()
            if any(first.startswith(prefix) for prefix in noise_prefixes):
                continue
            if self._is_noise_block(block):
                continue
            useful.append(block)
        if not useful:
            return ""
        if len(useful) >= 2 and self._looks_like_followup(useful[-1]) and self._looks_like_final_answer(useful[-2]):
            return f"{useful[-2]}\n\n{useful[-1]}"
        for block in reversed(useful):
            if self._looks_like_final_answer(block):
                return block
        return max(useful, key=len)

    def _is_noise_block(self, block: str) -> bool:
        stripped = block.strip()
        if not stripped:
            return True
        if re.fullmatch(r"[-=\s`]+", stripped):
            return True
        if stripped.lower().startswith(("llm running", "code run output", "tool_use")):
            return True
        return False

    def _looks_like_final_answer(self, block: str) -> bool:
        stripped = block.strip()
        if not stripped:
            return False
        if re.fullmatch(r"<[^>]+>", stripped):
            return False
        if re.fullmatch(r"#+\s*.+", stripped) and len(stripped.splitlines()) == 1:
            return False
        normalized = re.sub(r"[#>*`\-\s_]+", "", stripped)
        return len(normalized) >= 8

    def _looks_like_followup(self, block: str) -> bool:
        stripped = block.strip()
        if not stripped or len(stripped) > 120:
            return False
        markers = ("需要", "要不要", "是否", "如果你想", "要我", "吗", "呢")
        return any(marker in stripped for marker in markers)
