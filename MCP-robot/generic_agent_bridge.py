from __future__ import annotations

import asyncio
import logging
import os
import re
from pathlib import Path


logger = logging.getLogger("MCP_Robot.GenericAgent")


class GenericAgentBridge:
    def __init__(self, agent_root: Path, agent_python: Path) -> None:
        self.agent_root = agent_root
        self.agent_python = agent_python
        self.runner_script = Path(__file__).resolve().parent / "generic_agent_runner.py"

    async def run_root_command(self, command: str, *, timeout_seconds: int = 900) -> str:
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "utf-8"
        env["PYTHONUTF8"] = "1"
        process = await asyncio.create_subprocess_exec(
            str(self.agent_python),
            "-X",
            "utf8",
            str(self.runner_script),
            "--agent-root",
            str(self.agent_root),
            "--prompt",
            command.strip(),
            "--timeout",
            str(timeout_seconds),
            cwd=str(self.agent_root),
            env=env,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate()
        out_text = self._decode_output(stdout).strip()
        err_text = self._decode_output(stderr).strip()
        if process.returncode != 0:
            detail = err_text or out_text or f"GenericAgent exit code={process.returncode}"
            raise RuntimeError(detail)
        if err_text:
            logger.warning("GenericAgent stderr: %s", err_text)
        return self._extract_user_facing_output(out_text)

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
