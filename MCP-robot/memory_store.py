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
        scored_entries = self.scored_memories(limit=limit)
        if not scored_entries:
            return ""
        lines_out = ["潜意识观察（按时间衰减后的可信度排序）："]
        for item in scored_entries:
            score = float(item["score"])
            memory = SubconsciousMemory(
                timestamp=str(item["timestamp"]),
                source=str(item["source"]),
                summary=str(item["summary"]),
                confidence=float(item["confidence"]),
                importance=float(item["importance"]),
                tags=list(item.get("tags") or []),
            )
            tags = f" tags={','.join(memory.tags)}" if memory.tags else ""
            lines_out.append(
                f"- [{memory.timestamp}] score={score:.2f} confidence={memory.confidence:.2f}{tags} {memory.summary}"
            )
        return "\n".join(lines_out)

    def scored_memories(self, *, limit: int = 20) -> list[dict[str, object]]:
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
                recency_boost = 1.0
                image_like = memory.source.startswith(("napcat_image", "napcat_image_enriched", "vision:")) or any(
                    "image" in tag.lower() or tag in {"comic", "manhwa", "screenshot", "food", "meal"} for tag in memory.tags
                )
                if image_like:
                    if age_hours <= 0.2:
                        recency_boost = 2.6
                    elif age_hours <= 1.0:
                        recency_boost = 1.9
                    elif age_hours <= 6.0:
                        recency_boost = 1.35
                score = memory.confidence * memory.importance * decay * recency_boost
                scored_entries.append((score, memory))
            except Exception:
                continue
        scored_entries.sort(key=lambda item: item[0], reverse=True)
        output: list[dict[str, object]] = []
        for score, memory in scored_entries[: max(1, limit)]:
            output.append(
                {
                    "timestamp": memory.timestamp,
                    "source": memory.source,
                    "summary": memory.summary,
                    "confidence": memory.confidence,
                    "importance": memory.importance,
                    "tags": list(memory.tags),
                    "score": round(score, 6),
                }
            )
        return output
