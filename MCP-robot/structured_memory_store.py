from __future__ import annotations

import hashlib
import json
import threading
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _stable_id(prefix: str, *parts: str) -> str:
    digest = hashlib.sha1("||".join(parts).encode("utf-8")).hexdigest()[:20]
    return f"{prefix}:{digest}"


def _parse_ts(value: str) -> datetime | None:
    if not value:
        return None
    try:
        dt = datetime.fromisoformat(value)
    except Exception:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _display_ts(value: str) -> str:
    dt = _parse_ts(value)
    if dt is None:
        return value.strip()
    return dt.astimezone().strftime("%Y-%m-%d %H:%M")


@dataclass(slots=True)
class StructuredMemory:
    memory_id: str
    session_id: str
    memory_type: str
    text: str
    confidence: float
    source: str
    created_at: str
    freshness: str = "long"
    valid_from: str = ""
    valid_until: str = ""
    tags: list[str] = field(default_factory=list)
    evidence_doc_ids: list[str] = field(default_factory=list)

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "StructuredMemory":
        text = str(payload.get("text") or "").strip()
        session_id = str(payload.get("session_id") or "").strip()
        memory_type = str(payload.get("memory_type") or "").strip()
        created_at = str(payload.get("created_at") or "").strip()
        memory_id = str(payload.get("memory_id") or "").strip()
        if not memory_id:
            memory_id = _stable_id("smem", session_id, memory_type, text)
        return cls(
            memory_id=memory_id,
            session_id=session_id,
            memory_type=memory_type,
            text=text,
            confidence=max(0.0, min(1.0, float(payload.get("confidence") or 0.0))),
            source=str(payload.get("source") or "").strip(),
            created_at=created_at,
            freshness=str(payload.get("freshness") or "long").strip() or "long",
            valid_from=str(payload.get("valid_from") or "").strip(),
            valid_until=str(payload.get("valid_until") or "").strip(),
            tags=[str(item).strip() for item in (payload.get("tags") or []) if str(item).strip()],
            evidence_doc_ids=[str(item).strip() for item in (payload.get("evidence_doc_ids") or []) if str(item).strip()],
        )


@dataclass(slots=True)
class UserProfileFact:
    profile_id: str
    scope_id: str
    key: str
    value: str
    summary: str
    confidence: float
    source: str
    created_at: str
    updated_at: str
    tags: list[str] = field(default_factory=list)
    evidence_doc_ids: list[str] = field(default_factory=list)

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "UserProfileFact":
        scope_id = str(payload.get("scope_id") or "").strip()
        key = str(payload.get("key") or "").strip()
        value = str(payload.get("value") or "").strip()
        profile_id = str(payload.get("profile_id") or "").strip()
        if not profile_id:
            profile_id = _stable_id("profile", scope_id, key)
        created_at = str(payload.get("created_at") or "").strip()
        updated_at = str(payload.get("updated_at") or created_at).strip()
        return cls(
            profile_id=profile_id,
            scope_id=scope_id,
            key=key,
            value=value,
            summary=str(payload.get("summary") or "").strip(),
            confidence=max(0.0, min(1.0, float(payload.get("confidence") or 0.0))),
            source=str(payload.get("source") or "").strip(),
            created_at=created_at,
            updated_at=updated_at,
            tags=[str(item).strip() for item in (payload.get("tags") or []) if str(item).strip()],
            evidence_doc_ids=[str(item).strip() for item in (payload.get("evidence_doc_ids") or []) if str(item).strip()],
        )


class StructuredMemoryStore:
    def __init__(self, memory_file: Path, profile_file: Path) -> None:
        self.memory_file = memory_file
        self.profile_file = profile_file
        self.memory_file.parent.mkdir(parents=True, exist_ok=True)
        self.memory_file.touch(exist_ok=True)
        self.profile_file.touch(exist_ok=True)
        self._lock = threading.Lock()

    def remember_many(self, *, session_id: str, memories: list[dict[str, Any]], source: str) -> int:
        if not session_id or not memories:
            return 0
        existing = self._load_memories()
        now_iso = datetime.now().astimezone().isoformat()
        saved = 0
        for payload in memories:
            text = str(payload.get("text") or "").strip()
            memory_type = str(payload.get("type") or payload.get("memory_type") or "").strip()
            if not text or not memory_type:
                continue
            record = StructuredMemory.from_payload(
                {
                    "memory_id": str(payload.get("memory_id") or "").strip(),
                    "session_id": session_id,
                    "memory_type": memory_type,
                    "text": text,
                    "confidence": payload.get("confidence") or 0.0,
                    "source": source,
                    "created_at": str(payload.get("created_at") or now_iso),
                    "freshness": str(payload.get("freshness") or "long"),
                    "valid_from": str(payload.get("valid_from") or ""),
                    "valid_until": str(payload.get("valid_until") or ""),
                    "tags": payload.get("tags") or [],
                    "evidence_doc_ids": payload.get("evidence_doc_ids") or [],
                }
            )
            previous = existing.get(record.memory_id)
            if previous is not None and previous.confidence > record.confidence and previous.text == record.text:
                continue
            existing[record.memory_id] = record
            saved += 1
        if saved:
            self._rewrite_memories(existing.values())
        return saved

    def upsert_profile_facts(self, *, scope_id: str, facts: list[dict[str, Any]], source: str) -> int:
        if not scope_id or not facts:
            return 0
        existing = self._load_profiles()
        now_iso = datetime.now().astimezone().isoformat()
        updated = 0
        for payload in facts:
            key = str(payload.get("key") or "").strip()
            value = str(payload.get("value") or "").strip()
            summary = str(payload.get("summary") or "").strip()
            if not key or not value or not summary:
                continue
            incoming = UserProfileFact.from_payload(
                {
                    "scope_id": scope_id,
                    "key": key,
                    "value": value,
                    "summary": summary,
                    "confidence": payload.get("confidence") or 0.0,
                    "source": source,
                    "created_at": now_iso,
                    "updated_at": now_iso,
                    "tags": payload.get("tags") or [],
                    "evidence_doc_ids": payload.get("evidence_doc_ids") or [],
                }
            )
            previous = existing.get(incoming.profile_id)
            if previous is None:
                existing[incoming.profile_id] = incoming
                updated += 1
                continue
            merged_tags = sorted(set(previous.tags + incoming.tags))
            merged_evidence = sorted(set(previous.evidence_doc_ids + incoming.evidence_doc_ids))
            existing[incoming.profile_id] = UserProfileFact(
                profile_id=previous.profile_id,
                scope_id=previous.scope_id,
                key=previous.key,
                value=incoming.value if incoming.confidence >= previous.confidence else previous.value,
                summary=incoming.summary if incoming.confidence >= previous.confidence else previous.summary,
                confidence=max(previous.confidence, incoming.confidence),
                source=incoming.source or previous.source,
                created_at=previous.created_at or now_iso,
                updated_at=now_iso,
                tags=merged_tags,
                evidence_doc_ids=merged_evidence,
            )
            updated += 1
        if updated:
            self._rewrite_profiles(existing.values())
        return updated

    def build_context(self, *, session_id: str, profile_scope: str, profile_limit: int = 6, memory_limit: int = 6) -> str:
        lines: list[str] = []
        profiles = self.profile_facts(scope_id=profile_scope, limit=profile_limit)
        memories = self.active_memories(session_id=session_id, limit=memory_limit)
        if profiles:
            lines.append("稳定用户画像:")
            for item in profiles:
                updated_label = _display_ts(item.updated_at or item.created_at)
                lines.append(f"- [更新于 {updated_label}] {item.summary}")
        if memories:
            lines.append("结构化记忆:")
            for item in memories:
                created_label = _display_ts(item.created_at)
                lines.append(f"- [{item.memory_type} | 记录于 {created_label}] {item.text}")
        return "\n".join(lines)

    def profile_facts(self, *, scope_id: str, limit: int = 12) -> list[UserProfileFact]:
        facts = [item for item in self._load_profiles().values() if item.scope_id == scope_id and item.summary]
        facts.sort(
            key=lambda item: (
                item.confidence,
                _parse_ts(item.updated_at) or datetime.min.replace(tzinfo=timezone.utc),
            ),
            reverse=True,
        )
        return facts[: max(1, limit)]

    def active_memories(self, *, session_id: str, limit: int = 12) -> list[StructuredMemory]:
        now = datetime.now(timezone.utc)
        items = [item for item in self._load_memories().values() if item.session_id == session_id and item.text]
        active: list[StructuredMemory] = []
        for item in items:
            valid_until = _parse_ts(item.valid_until)
            if valid_until is not None and valid_until < now:
                continue
            active.append(item)
        active.sort(
            key=lambda item: (
                item.confidence,
                _parse_ts(item.created_at) or datetime.min.replace(tzinfo=timezone.utc),
            ),
            reverse=True,
        )
        return active[: max(1, limit)]

    def memory_count(self) -> int:
        return len(self._load_memories())

    def profile_count(self) -> int:
        return len(self._load_profiles())

    def _load_memories(self) -> dict[str, StructuredMemory]:
        with self._lock:
            lines = self.memory_file.read_text(encoding="utf-8").splitlines()
        items: dict[str, StructuredMemory] = {}
        for line in lines:
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                item = StructuredMemory.from_payload(payload)
            except Exception:
                continue
            if item.memory_id:
                items[item.memory_id] = item
        return items

    def _load_profiles(self) -> dict[str, UserProfileFact]:
        with self._lock:
            lines = self.profile_file.read_text(encoding="utf-8").splitlines()
        items: dict[str, UserProfileFact] = {}
        for line in lines:
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                item = UserProfileFact.from_payload(payload)
            except Exception:
                continue
            if item.profile_id:
                items[item.profile_id] = item
        return items

    def _rewrite_memories(self, records: Any) -> None:
        serialized = [json.dumps(asdict(item), ensure_ascii=False) for item in records]
        with self._lock:
            self.memory_file.write_text("\n".join(serialized) + ("\n" if serialized else ""), encoding="utf-8")

    def _rewrite_profiles(self, records: Any) -> None:
        serialized = [json.dumps(asdict(item), ensure_ascii=False) for item in records]
        with self._lock:
            self.profile_file.write_text("\n".join(serialized) + ("\n" if serialized else ""), encoding="utf-8")
