from __future__ import annotations

import json
import math
import threading
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path


@dataclass(slots=True)
class SubconsciousMemory:
    timestamp: str
    source: str
    summary: str
    confidence: float
    importance: float
    tags: list[str] = field(default_factory=list)


class SubconsciousMemoryStore:
    def __init__(self, file_path: Path, half_life_hours: float = 72.0) -> None:
        self.file_path = file_path
        self.half_life_hours = max(1.0, half_life_hours)
        self.file_path.parent.mkdir(parents=True, exist_ok=True)
        self.file_path.touch(exist_ok=True)
        self._lock = threading.Lock()

    def add(self, entry: SubconsciousMemory) -> None:
        line = json.dumps(asdict(entry), ensure_ascii=False)
        with self._lock:
            with self.file_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")

    def remember(
        self,
        *,
        source: str,
        summary: str,
        confidence: float,
        importance: float,
        tags: list[str] | None = None,
    ) -> None:
        self.add(
            SubconsciousMemory(
                timestamp=datetime.now().astimezone().isoformat(),
                source=source,
                summary=summary.strip(),
                confidence=max(0.0, min(1.0, confidence)),
                importance=max(0.0, min(1.0, importance)),
                tags=tags or [],
            )
        )

    def build_context(self, limit: int = 6) -> str:
        now = datetime.now(timezone.utc)
        scored_entries: list[tuple[float, SubconsciousMemory]] = []
        with self._lock:
            lines = self.file_path.read_text(encoding="utf-8").splitlines()
        for line in lines:
            if not line.strip():
                continue
            try:
                raw = json.loads(line)
                memory = SubconsciousMemory(**raw)
                ts = datetime.fromisoformat(memory.timestamp)
                if ts.tzinfo is None:
                    ts = ts.replace(tzinfo=timezone.utc)
                age_hours = max(0.0, (now - ts.astimezone(timezone.utc)).total_seconds() / 3600)
                decay = math.exp(-age_hours / self.half_life_hours)
                score = memory.confidence * memory.importance * decay
                scored_entries.append((score, memory))
            except Exception:
                continue
        if not scored_entries:
            return ""
        scored_entries.sort(key=lambda item: item[0], reverse=True)
        selected = scored_entries[:limit]
        lines_out = ["潜意识观察（按时间衰减后的可信度排序）："]
        for score, memory in selected:
            tags = f" tags={','.join(memory.tags)}" if memory.tags else ""
            lines_out.append(
                f"- [{memory.timestamp}] score={score:.2f} confidence={memory.confidence:.2f}{tags} {memory.summary}"
            )
        return "\n".join(lines_out)
