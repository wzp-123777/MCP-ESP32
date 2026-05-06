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
    provider: str = "openai"


@dataclass(frozen=True, slots=True)
class EmbeddingConfig:
    api_key: str
    endpoint: str
    model: str
    dimension: int = 1024
    timeout_seconds: float = 60.0


@dataclass(frozen=True, slots=True)
class ToolApiConfig:
    seniverse_key: str
    amap_key: str
    quark_search_api_key: str
    quark_search_agent_id: str
    quark_search_agent_version: str
    quark_search_workspace_id: str


@dataclass(frozen=True, slots=True)
class DoubaoDialogConfig:
    enabled: bool
    app_id: str
    app_key: str
    access_token: str
    resource_id: str = "volc.speech.dialog"
    ws_url: str = "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"
    bot_name: str = "豆包"
    system_role: str = "你是一个简洁、自然的中文语音助手。回答要短，适合直接朗读。"
    tts_speaker: str = "zh_female_vv_jupiter_bigtts"
    tts_format: str = "pcm_s16le"
    tts_sample_rate: int = 24000
    tts_channel: int = 1
    timeout_seconds: float = 30.0
    audio_chunk_ms: int = 100
    vad_tail_silence_ms: int = 1800
    output_flush_ms: int = 250
    persona_dir: Path = BASE_DIR / "data" / "personas"
    voice_preset_file: Path = BASE_DIR / "data" / "voice_presets.json"


@dataclass(frozen=True, slots=True)
class TTSPreset:
    voice: str
    style_prompt: str


TTS_PRESETS: dict[str, TTSPreset] = {
    "clear_female": TTSPreset(
        voice="茉莉",
        style_prompt="清脆女声，悠扬高雅，语速适中，吐字清楚，语气温柔自然",
    ),
    "sweet_female": TTSPreset(
        voice="冰糖",
        style_prompt="甜美女声，轻快亲切，语速略快但吐字清晰",
    ),
    "soft_female": TTSPreset(
        voice="Chloe",
        style_prompt="柔和女声，安静温暖，语速适中偏慢，适合陪伴式回答",
    ),
    "bright_female": TTSPreset(
        voice="Mia",
        style_prompt="明亮女声，清爽活泼，语气自然，短句有停顿",
    ),
    "calm_male": TTSPreset(
        voice="白桦",
        style_prompt="沉稳男声，清晰可靠，语速适中，语气平和",
    ),
}


@dataclass(frozen=True, slots=True)
class AppConfig:
    language_model: ModelConfig
    tool_model: ModelConfig
    vision_model: ModelConfig
    vision_highres_model: ModelConfig
    asr_model: ModelConfig
    tts_model: ModelConfig
    context_embedding: EmbeddingConfig
    asr_language: str
    tts_voice: str
    tts_style_prompt: str
    generic_agent_root: Path
    generic_agent_python: Path
    project_root: Path
    data_dir: Path
    subconscious_file: Path
    trace_log_file: Path
    runtime_log_file: Path
    tool_api: ToolApiConfig
    doubao_dialog: DoubaoDialogConfig
    subconscious_half_life_hours: float = 72.0
    qq_summary_max_chars: int = 160
    shared_session_id: str = "shared-context:main"
    context_recent_turns: int = 8
    context_rag_hits: int = 5
    memory_hot_turns: int = 8
    memory_summary_min_turns: int = 6
    memory_summary_batch_turns: int = 12
    conversation_cache_size: int = 128
    image_reply_relevance_threshold: int = 18
    offline_gap_analysis_enabled: bool = True
    offline_gap_threshold_minutes: int = 10
    offline_gap_recent_entries: int = 8
    high_risk_approval_enabled: bool = True
    mcp_facade_enabled: bool = True
    mcp_mount_path: str = "/mcp"
    esp32_realtime_mode: bool = True
    esp32_realtime_skip_temporal_context: bool = True
    esp32_realtime_skip_semantic_rag: bool = True
    esp32_realtime_fast_chat_max_chars: int = 120


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
        str(BASE_DIR / "venv" / "Scripts" / "python.exe"),
        str(agent_root / "venv" / "Scripts" / "python.exe"),
        str(agent_root / ".venv" / "Scripts" / "python.exe"),
        sys.executable,
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


def _hydrate_windows_persistent_env() -> None:
    if os.name != "nt":
        return
    try:
        import winreg
    except Exception:
        return
    keys = (
        (winreg.HKEY_CURRENT_USER, "Environment"),
        (winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
    )
    for root, path in keys:
        try:
            handle = winreg.OpenKey(root, path)
        except OSError:
            continue
        with handle:
            index = 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(handle, index)
                except OSError:
                    break
                index += 1
                if name in os.environ or not isinstance(value, str) or not value:
                    continue
                os.environ[name] = value


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


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name, "").strip().lower()
    if not raw:
        return default
    return raw not in {"0", "false", "no", "off"}


def load_config() -> AppConfig:
    _hydrate_windows_persistent_env()
    generic_agent_root = Path(os.getenv("GENERIC_AGENT_ROOT", r"D:\esp32\GenericAgent")).resolve()
    mcp_robot_keys = _load_python_module(BASE_DIR / "local_keys.py")
    generic_agent_keys = _load_python_module(generic_agent_root / "mykey.py")

    mimo_api_key = _pick_value(
        ["MIMO_API_KEY"],
        mcp_robot_keys,
        ["MIMO_API_KEY", "mimo_api_key"],
    ) or _pick_value(
        ["MIMO_API_KEY"],
        generic_agent_keys,
        ["MIMO_API_KEY", "mimo_api_key"],
    )
    mimo_base_url = os.getenv(
        "MIMO_BASE_URL",
        mcp_robot_keys.get("MIMO_BASE_URL")
        or mcp_robot_keys.get("mimo_base_url")
        or generic_agent_keys.get("MIMO_BASE_URL")
        or generic_agent_keys.get("mimo_base_url")
        or "https://api.xiaomimimo.com/v1",
    ).strip()
    tts_preset_name = os.getenv("MIMO_TTS_PRESET", "clear_female").strip()
    tts_preset = TTS_PRESETS.get(tts_preset_name, TTS_PRESETS["clear_female"])
    tts_voice = os.getenv("MIMO_TTS_VOICE", tts_preset.voice).strip()
    tts_style_prompt = os.getenv("MIMO_TTS_STYLE_PROMPT", tts_preset.style_prompt).strip()

    qwen_api_key = _pick_value(
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
        mcp_robot_keys,
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
    ) or _pick_value(
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
        generic_agent_keys,
        ["DASHSCOPE_API_KEY", "QWEN_API_KEY"],
    )
    qwen_base_url = os.getenv(
        "QWEN_BASE_URL",
        mcp_robot_keys.get("QWEN_BASE_URL")
        or mcp_robot_keys.get("qwen_base_url")
        or generic_agent_keys.get("QWEN_BASE_URL")
        or generic_agent_keys.get("qwen_base_url")
        or "https://dashscope.aliyuncs.com/compatible-mode/v1",
    ).strip()
    ark_api_key = os.getenv("ARK_API_KEY", "").strip()
    ark_base_url = os.getenv("ARK_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3").strip()
    default_language_api_key = ark_api_key or mimo_api_key
    default_language_base_url = ark_base_url if ark_api_key else mimo_base_url
    default_language_model = os.getenv(
        "ARK_LANGUAGE_MODEL",
        os.getenv("MIMO_LANGUAGE_MODEL", "mimo-v2.5-pro"),
    ).strip()
    default_tool_model = os.getenv(
        "ARK_TOOL_MODEL",
        os.getenv("MIMO_TOOL_MODEL", default_language_model),
    ).strip()
    doubao_dialog_app_id = os.getenv("DOUBAO_DIALOG_APP_ID", "").strip()
    doubao_dialog_app_key = _pick_value(
        ["DOUBAO_DIALOG_APP_KEY"],
        mcp_robot_keys,
        ["DOUBAO_DIALOG_APP_KEY", "doubao_dialog_app_key"],
    ) or _pick_value(
        ["DOUBAO_DIALOG_APP_KEY"],
        generic_agent_keys,
        ["DOUBAO_DIALOG_APP_KEY", "doubao_dialog_app_key"],
    )
    doubao_dialog_access_token = os.getenv("DOUBAO_DIALOG_ACCESS_TOKEN", "").strip()
    esp32_dialog_enabled = _env_bool("ESP32_DOUBAO_DIALOG_ENABLED", True)

    data_dir = Path(os.getenv("MCP_ROBOT_DATA_DIR", str(BASE_DIR / "data"))).resolve()
    subconscious_file = data_dir / "subconscious_memory.jsonl"
    trace_log_file = data_dir / "message_trace.log"
    runtime_log_file = data_dir / "runtime.log"
    project_root = Path(os.getenv("MCP_ROBOT_PROJECT_ROOT", r"D:\esp32")).resolve()
    generic_agent_python = _detect_python(generic_agent_root)

    return AppConfig(
        language_model=ModelConfig(
            api_key=os.getenv("LANGUAGE_API_KEY", default_language_api_key).strip(),
            base_url=os.getenv("LANGUAGE_BASE_URL", default_language_base_url).strip(),
            model=os.getenv("LANGUAGE_MODEL", default_language_model).strip(),
            timeout_seconds=float(os.getenv("LANGUAGE_TIMEOUT_SECONDS", os.getenv("MIMO_TIMEOUT_SECONDS", "120"))),
        ),
        tool_model=ModelConfig(
            api_key=os.getenv("TOOL_MODEL_API_KEY", default_language_api_key).strip(),
            base_url=os.getenv("TOOL_MODEL_BASE_URL", default_language_base_url).strip(),
            model=os.getenv("TOOL_MODEL", default_tool_model).strip(),
            timeout_seconds=float(os.getenv("TOOL_MODEL_TIMEOUT_SECONDS", os.getenv("MIMO_TIMEOUT_SECONDS", "120"))),
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
            model=os.getenv("VISION_HIGHRES_MODEL", "qwen3.6-plus").strip(),
            timeout_seconds=float(os.getenv("VISION_HIGHRES_TIMEOUT_SECONDS", "120")),
        ),
        asr_model=ModelConfig(
            api_key=os.getenv("ASR_API_KEY", qwen_api_key).strip(),
            base_url=os.getenv("ASR_BASE_URL", qwen_base_url).strip(),
            model=os.getenv("ASR_MODEL", "qwen3-asr-flash").strip(),
            timeout_seconds=float(os.getenv("ASR_TIMEOUT_SECONDS", "120")),
        ),
        # 默认使用 Xiaomi MiMo 2.5 TTS，走 OpenAI-compatible /chat/completions 路由。
        # 若切到 mimo-v2.5-tts-voicedesign，MIMO_TTS_STYLE_PROMPT 会作为 user 音色设计描述，
        # 服务端不会再传内置 voice 参数；assistant content 只保留实际朗读文本。
        tts_model=ModelConfig(
            api_key=os.getenv("TTS_API_KEY", os.getenv("MIMO_TTS_API_KEY", mimo_api_key)).strip(),
            base_url=os.getenv("TTS_BASE_URL", os.getenv("MIMO_TTS_BASE_URL", mimo_base_url)).strip(),
            model=os.getenv("TTS_MODEL", os.getenv("MIMO_TTS_MODEL", "mimo-v2.5-tts")).strip(),
            timeout_seconds=float(os.getenv("TTS_TIMEOUT_SECONDS", os.getenv("MIMO_TTS_TIMEOUT_SECONDS", os.getenv("MIMO_TIMEOUT_SECONDS", "120")))),
        ),
        context_embedding=EmbeddingConfig(
            api_key=os.getenv("CONTEXT_EMBEDDING_API_KEY", qwen_api_key).strip(),
            endpoint=os.getenv(
                "CONTEXT_EMBEDDING_ENDPOINT",
                "https://dashscope.aliyuncs.com/api/v1/services/embeddings/multimodal-embedding/multimodal-embedding",
            ).strip(),
            model=os.getenv("CONTEXT_EMBEDDING_MODEL", "qwen3-vl-embedding").strip(),
            dimension=int(os.getenv("CONTEXT_EMBEDDING_DIMENSION", "1024")),
            timeout_seconds=float(os.getenv("CONTEXT_EMBEDDING_TIMEOUT_SECONDS", "60")),
        ),
        asr_language=os.getenv("ASR_LANGUAGE", "zh").strip(),
        tts_voice=tts_voice,
        tts_style_prompt=tts_style_prompt,
        generic_agent_root=generic_agent_root,
        generic_agent_python=generic_agent_python,
        project_root=project_root,
        data_dir=data_dir,
        subconscious_file=subconscious_file,
        trace_log_file=trace_log_file,
        runtime_log_file=runtime_log_file,
        tool_api=ToolApiConfig(
            seniverse_key=_pick_value(["SENIVERSE_API_KEY"], mcp_robot_keys, ["SENIVERSE_API_KEY", "seniverse_api_key"])
            or _pick_value(["SENIVERSE_API_KEY"], generic_agent_keys, ["SENIVERSE_API_KEY", "seniverse_api_key"]),
            amap_key=_pick_value(["AMAP_API_KEY"], mcp_robot_keys, ["AMAP_API_KEY", "amap_api_key"])
            or _pick_value(["AMAP_API_KEY"], generic_agent_keys, ["AMAP_API_KEY", "amap_api_key"]),
            quark_search_api_key=_pick_value(
                ["BAILIAN_SEARCH_API_KEY", "DASHSCOPE_API_KEY", "QWEN_API_KEY"],
                mcp_robot_keys,
                ["BAILIAN_SEARCH_API_KEY", "DASHSCOPE_API_KEY", "QWEN_API_KEY"],
            )
            or _pick_value(
                ["BAILIAN_SEARCH_API_KEY", "DASHSCOPE_API_KEY", "QWEN_API_KEY"],
                generic_agent_keys,
                ["BAILIAN_SEARCH_API_KEY", "DASHSCOPE_API_KEY", "QWEN_API_KEY"],
            )
            or qwen_api_key,
            quark_search_agent_id=_pick_value(
                ["BAILIAN_SEARCH_AGENT_ID", "QUARK_SEARCH_AGENT_ID"],
                mcp_robot_keys,
                ["BAILIAN_SEARCH_AGENT_ID", "QUARK_SEARCH_AGENT_ID"],
            )
            or _pick_value(
                ["BAILIAN_SEARCH_AGENT_ID", "QUARK_SEARCH_AGENT_ID"],
                generic_agent_keys,
                ["BAILIAN_SEARCH_AGENT_ID", "QUARK_SEARCH_AGENT_ID"],
            ),
            quark_search_agent_version=_pick_value(
                ["BAILIAN_SEARCH_AGENT_VERSION", "QUARK_SEARCH_AGENT_VERSION"],
                mcp_robot_keys,
                ["BAILIAN_SEARCH_AGENT_VERSION", "QUARK_SEARCH_AGENT_VERSION"],
            )
            or _pick_value(
                ["BAILIAN_SEARCH_AGENT_VERSION", "QUARK_SEARCH_AGENT_VERSION"],
                generic_agent_keys,
                ["BAILIAN_SEARCH_AGENT_VERSION", "QUARK_SEARCH_AGENT_VERSION"],
            )
            or "release",
            quark_search_workspace_id=_pick_value(
                ["BAILIAN_WORKSPACE_ID", "DASHSCOPE_WORKSPACE_ID", "QUARK_SEARCH_WORKSPACE_ID"],
                mcp_robot_keys,
                ["BAILIAN_WORKSPACE_ID", "DASHSCOPE_WORKSPACE_ID", "QUARK_SEARCH_WORKSPACE_ID"],
            )
            or _pick_value(
                ["BAILIAN_WORKSPACE_ID", "DASHSCOPE_WORKSPACE_ID", "QUARK_SEARCH_WORKSPACE_ID"],
                generic_agent_keys,
                ["BAILIAN_WORKSPACE_ID", "DASHSCOPE_WORKSPACE_ID", "QUARK_SEARCH_WORKSPACE_ID"],
            ),
        ),
        doubao_dialog=DoubaoDialogConfig(
            enabled=esp32_dialog_enabled,
            app_id=doubao_dialog_app_id,
            app_key=doubao_dialog_app_key,
            access_token=doubao_dialog_access_token,
            resource_id=os.getenv("DOUBAO_DIALOG_RESOURCE_ID", "volc.speech.dialog").strip(),
            ws_url=os.getenv("DOUBAO_DIALOG_WS_URL", "wss://openspeech.bytedance.com/api/v3/realtime/dialogue").strip(),
            bot_name=os.getenv("DOUBAO_DIALOG_BOT_NAME", "豆包").strip(),
            system_role=os.getenv(
                "DOUBAO_DIALOG_SYSTEM_ROLE",
                "你是一个简洁、自然的中文语音助手。回答要短，适合直接朗读。",
            ).strip(),
            tts_speaker=os.getenv("DOUBAO_DIALOG_TTS_SPEAKER", "zh_female_vv_jupiter_bigtts").strip(),
            tts_format=os.getenv("DOUBAO_DIALOG_TTS_FORMAT", "pcm_s16le").strip(),
            tts_sample_rate=max(8000, int(os.getenv("DOUBAO_DIALOG_TTS_SAMPLE_RATE", "24000"))),
            tts_channel=max(1, int(os.getenv("DOUBAO_DIALOG_TTS_CHANNEL", "1"))),
            timeout_seconds=float(os.getenv("DOUBAO_DIALOG_TIMEOUT_SECONDS", "30")),
            audio_chunk_ms=max(20, int(os.getenv("DOUBAO_DIALOG_AUDIO_CHUNK_MS", "40"))),
            # The realtime dialog ASR endpoint currently reports eos_silence_timeout=1500.
            # File-style ESP32 uploads need an explicit silence tail long enough for short
            # utterances; otherwise the service may emit ClientLackDataError/idle timeout.
            vad_tail_silence_ms=max(0, int(os.getenv("DOUBAO_DIALOG_VAD_TAIL_SILENCE_MS", "1600"))),
            output_flush_ms=max(120, int(os.getenv("DOUBAO_DIALOG_OUTPUT_FLUSH_MS", "160"))),
            persona_dir=Path(os.getenv("MCP_PERSONA_DIR", str(data_dir / "personas"))).resolve(),
            voice_preset_file=Path(os.getenv("MCP_VOICE_PRESET_FILE", str(data_dir / "voice_presets.json"))).resolve(),
        ),
        subconscious_half_life_hours=float(os.getenv("SUBCONSCIOUS_HALF_LIFE_HOURS", "72")),
        qq_summary_max_chars=int(os.getenv("QQ_SUMMARY_MAX_CHARS", "160")),
        shared_session_id=os.getenv("SHARED_CONTEXT_SESSION_ID", "shared-context:main").strip(),
        context_recent_turns=int(os.getenv("CONTEXT_RECENT_TURNS", "8")),
        context_rag_hits=int(os.getenv("CONTEXT_RAG_HITS", "5")),
        memory_hot_turns=int(os.getenv("MEMORY_HOT_TURNS", "8")),
        memory_summary_min_turns=int(os.getenv("MEMORY_SUMMARY_MIN_TURNS", "6")),
        memory_summary_batch_turns=int(os.getenv("MEMORY_SUMMARY_BATCH_TURNS", "12")),
        conversation_cache_size=int(os.getenv("CONVERSATION_CACHE_SIZE", "128")),
        image_reply_relevance_threshold=max(0, min(100, int(os.getenv("IMAGE_REPLY_RELEVANCE_THRESHOLD", "18")))),
        offline_gap_analysis_enabled=_env_bool("OFFLINE_GAP_ANALYSIS_ENABLED", True),
        offline_gap_threshold_minutes=max(1, int(os.getenv("OFFLINE_GAP_THRESHOLD_MINUTES", "10"))),
        offline_gap_recent_entries=max(4, int(os.getenv("OFFLINE_GAP_RECENT_ENTRIES", "8"))),
        high_risk_approval_enabled=_env_bool("HIGH_RISK_APPROVAL_ENABLED", True),
        mcp_facade_enabled=_env_bool("MCP_FACADE_ENABLED", True),
        mcp_mount_path=os.getenv("MCP_MOUNT_PATH", "/mcp").strip() or "/mcp",
        esp32_realtime_mode=_env_bool("ESP32_REALTIME_MODE", True),
        esp32_realtime_skip_temporal_context=_env_bool("ESP32_REALTIME_SKIP_TEMPORAL_CONTEXT", True),
        esp32_realtime_skip_semantic_rag=_env_bool("ESP32_REALTIME_SKIP_SEMANTIC_RAG", True),
        esp32_realtime_fast_chat_max_chars=max(24, int(os.getenv("ESP32_REALTIME_FAST_CHAT_MAX_CHARS", "120"))),
    )
