from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BASE_DIR = Path(__file__).resolve().parent


@dataclass(frozen=True, slots=True)
class ModelConfig:
    api_key: str
    base_url: str
    model: str
    timeout_seconds: float = 90.0


@dataclass(frozen=True, slots=True)
class ToolApiConfig:
    seniverse_key: str
    tavily_key: str
    amap_key: str


@dataclass(frozen=True, slots=True)
class AppConfig:
    language_model: ModelConfig
    tool_model: ModelConfig
    vision_model: ModelConfig
    vision_highres_model: ModelConfig
    tts_model: ModelConfig
    tts_voice: str
    generic_agent_root: Path
    generic_agent_python: Path
    project_root: Path
    data_dir: Path
    subconscious_file: Path
    trace_log_file: Path
    runtime_log_file: Path
    tool_api: ToolApiConfig
    subconscious_half_life_hours: float = 72.0
    qq_summary_max_chars: int = 160


def _detect_python(agent_root: Path) -> Path:
    explicit = os.getenv("GENERIC_AGENT_PYTHON", "").strip()
    if explicit:
        path = Path(explicit)
        if path.exists():
            return path.resolve()

    def _has_modules(path: Path, modules: tuple[str, ...]) -> bool:
        try:
            probe = subprocess.run(
                [
                    str(path),
                    "-c",
                    (
                        "import importlib.util, sys; "
                        "missing=[m for m in sys.argv[1:] if importlib.util.find_spec(m) is None]; "
                        "raise SystemExit(0 if not missing else 1)"
                    ),
                    *modules,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=8,
                check=False,
            )
            return probe.returncode == 0
        except Exception:
            return False

    candidates = [
        str(agent_root / "venv" / "Scripts" / "python.exe"),
        str(agent_root / ".venv" / "Scripts" / "python.exe"),
        sys.executable,
        r"D:\python2\python.exe",
    ]

    for raw in candidates:
        if not raw:
            continue
        path = Path(raw)
        if path.exists() and _has_modules(path, ("requests", "urllib3")):
            return path.resolve()

    for raw in candidates:
        if not raw:
            continue
        path = Path(raw)
        if path.exists():
            return path.resolve()
    return Path(sys.executable).resolve()


def _load_python_module(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        return {}
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return {name: getattr(module, name) for name in dir(module) if not name.startswith("_")}


def _pick_value(env_names: list[str], fallback: dict[str, Any], fallback_names: list[str]) -> str:
    for name in env_names:
        value = os.getenv(name, "").strip()
        if value:
            return value
    for name in fallback_names:
        value = fallback.get(name)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return ""


def load_config() -> AppConfig:
    generic_agent_root = Path(os.getenv("GENERIC_AGENT_ROOT", r"D:\esp32\GenericAgent")).resolve()
    generic_agent_keys = _load_python_module(generic_agent_root / "mykey.py")

    mimo_api_key = _pick_value(
        ["MIMO_API_KEY"],
        generic_agent_keys,
        ["MIMO_API_KEY", "mimo_api_key"],
    )
    mimo_base_url = os.getenv(
        "MIMO_BASE_URL",
        generic_agent_keys.get("MIMO_BASE_URL") or generic_agent_keys.get("mimo_base_url") or "https://api.xiaomimimo.com/v1",
    ).strip()

    qwen_api_key = _pick_value(
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
        generic_agent_keys,
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
    )
    qwen_base_url = os.getenv("QWEN_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1").strip()

    data_dir = Path(os.getenv("MCP_ROBOT_DATA_DIR", str(BASE_DIR / "data"))).resolve()
    subconscious_file = data_dir / "subconscious_memory.jsonl"
    trace_log_file = data_dir / "message_trace.log"
    runtime_log_file = data_dir / "runtime.log"
    project_root = Path(os.getenv("MCP_ROBOT_PROJECT_ROOT", r"D:\esp32")).resolve()
    generic_agent_python = _detect_python(generic_agent_root)

    return AppConfig(
        language_model=ModelConfig(
            api_key=mimo_api_key,
            base_url=mimo_base_url,
            model=os.getenv("MIMO_LANGUAGE_MODEL", "mimo-v2-pro").strip(),
            timeout_seconds=float(os.getenv("MIMO_TIMEOUT_SECONDS", "120")),
        ),
        tool_model=ModelConfig(
            api_key=mimo_api_key,
            base_url=mimo_base_url,
            model=os.getenv("MIMO_TOOL_MODEL", "mimo-v2-pro").strip(),
            timeout_seconds=float(os.getenv("MIMO_TIMEOUT_SECONDS", "120")),
        ),
        # 视觉链路默认切到 Qwen VL。
        # 低频环境观察优先速度，高清检查优先细节；都保留环境变量覆盖入口。
        vision_model=ModelConfig(
            api_key=os.getenv("VISION_LOWRES_API_KEY", qwen_api_key).strip(),
            base_url=os.getenv("VISION_LOWRES_BASE_URL", qwen_base_url).strip(),
            model=os.getenv("VISION_LOWRES_MODEL", "qwen3-vl-flash").strip(),
            timeout_seconds=float(os.getenv("VISION_LOWRES_TIMEOUT_SECONDS", "120")),
        ),
        vision_highres_model=ModelConfig(
            api_key=os.getenv("VISION_HIGHRES_API_KEY", qwen_api_key).strip(),
            base_url=os.getenv("VISION_HIGHRES_BASE_URL", os.getenv("VISION_LOWRES_BASE_URL", qwen_base_url)).strip(),
            model=os.getenv("VISION_HIGHRES_MODEL", "qwen3-vl-plus").strip(),
            timeout_seconds=float(os.getenv("VISION_HIGHRES_TIMEOUT_SECONDS", "120")),
        ),
        # 2026-04-14 DashScope 公开文档里可直接调用的 Qwen3.5-Omni 文本/音频模型
        # 使用的是 qwen3.5-omni-plus；如果你有别名或自建路由，可继续用环境变量覆盖。
        tts_model=ModelConfig(
            api_key=qwen_api_key,
            base_url=qwen_base_url,
            model=os.getenv("QWEN_TTS_MODEL", "qwen3.5-omni-plus").strip(),
            timeout_seconds=float(os.getenv("QWEN_TIMEOUT_SECONDS", "120")),
        ),
        tts_voice=os.getenv("QWEN_TTS_VOICE", "Cherry").strip(),
        generic_agent_root=generic_agent_root,
        generic_agent_python=generic_agent_python,
        project_root=project_root,
        data_dir=data_dir,
        subconscious_file=subconscious_file,
        trace_log_file=trace_log_file,
        runtime_log_file=runtime_log_file,
        tool_api=ToolApiConfig(
            seniverse_key=os.getenv("SENIVERSE_API_KEY", "").strip(),
            tavily_key=os.getenv("TAVILY_API_KEY", "").strip(),
            amap_key=os.getenv("AMAP_API_KEY", "").strip(),
        ),
        subconscious_half_life_hours=float(os.getenv("SUBCONSCIOUS_HALF_LIFE_HOURS", "72")),
        qq_summary_max_chars=int(os.getenv("QQ_SUMMARY_MAX_CHARS", "160")),
    )
