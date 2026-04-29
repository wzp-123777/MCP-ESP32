from __future__ import annotations

import hashlib
import json
import math
import re
import threading
from collections import OrderedDict
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


_TOKEN_RE = re.compile(r"[a-z0-9_]+", re.IGNORECASE)
_CJK_RE = re.compile(r"[\u4e00-\u9fff]+")


def _stable_document_id(prefix: str, *parts: str) -> str:
    digest = hashlib.sha1("||".join(parts).encode("utf-8")).hexdigest()[:20]
    return f"{prefix}:{digest}"


@dataclass(slots=True)
class ConversationEntry:
    timestamp: str
    session_id: str
    source: str
    kind: str
    user_text: str = ""
    assistant_text: str = ""
    summary: str = ""
    tags: list[str] = field(default_factory=list)
    entry_id: str = ""

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "ConversationEntry":
        user_text = str(payload.get("user_text") or "")
        assistant_text = str(payload.get("assistant_text") or "")
        summary = str(payload.get("summary") or "")
        session_id = str(payload.get("session_id") or "")
        timestamp = str(payload.get("timestamp") or "")
        source = str(payload.get("source") or "")
        kind = str(payload.get("kind") or "turn")
        entry_id = str(payload.get("entry_id") or "").strip()
        if not entry_id:
            entry_id = _stable_document_id(
                "entry",
                session_id,
                timestamp,
                source,
                kind,
                user_text,
                assistant_text,
                summary,
            )
        return cls(
            timestamp=timestamp,
            session_id=session_id,
            source=source,
            kind=kind,
            user_text=user_text,
            assistant_text=assistant_text,
            summary=summary,
            tags=list(payload.get("tags") or []),
            entry_id=entry_id,
        )

    def searchable_text(self) -> str:
        return "\n".join(
            part
            for part in (self.user_text, self.assistant_text, self.summary, " ".join(self.tags))
            if part
        ).strip()

    def semantic_text(self) -> str:
        if self.kind == "turn":
            return f"用户: {self.user_text}\n助手: {self.assistant_text}".strip()
        return self.summary.strip()


@dataclass(slots=True)
class SummarySnapshot:
    timestamp: str
    session_id: str
    start_index: int
    end_index: int
    start_timestamp: str
    end_timestamp: str
    summary: str
    tags: list[str] = field(default_factory=list)
    source: str = "tool_memory_worker"
    summary_id: str = ""

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "SummarySnapshot":
        session_id = str(payload.get("session_id") or "")
        timestamp = str(payload.get("timestamp") or "")
        summary = str(payload.get("summary") or "")
        summary_id = str(payload.get("summary_id") or "").strip()
        if not summary_id:
            summary_id = _stable_document_id(
                "summary",
                session_id,
                timestamp,
                str(payload.get("start_index") or 0),
                str(payload.get("end_index") or 0),
                summary,
            )
        return cls(
            timestamp=timestamp,
            session_id=session_id,
            start_index=int(payload.get("start_index") or 0),
            end_index=int(payload.get("end_index") or 0),
            start_timestamp=str(payload.get("start_timestamp") or ""),
            end_timestamp=str(payload.get("end_timestamp") or ""),
            summary=summary,
            tags=list(payload.get("tags") or []),
            source=str(payload.get("source") or "tool_memory_worker"),
            summary_id=summary_id,
        )

    def searchable_text(self) -> str:
        return "\n".join(part for part in (self.summary, " ".join(self.tags)) if part).strip()

    def semantic_text(self) -> str:
        return self.summary.strip()


@dataclass(slots=True)
class EmbeddingRecord:
    doc_id: str
    session_id: str
    kind: str
    source_kind: str
    timestamp: str
    text: str
    tags: list[str] = field(default_factory=list)
    model: str = ""
    dimension: int = 0
    vector: list[float] = field(default_factory=list)

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "EmbeddingRecord":
        vector = [float(value) for value in (payload.get("vector") or [])]
        return cls(
            doc_id=str(payload.get("doc_id") or ""),
            session_id=str(payload.get("session_id") or ""),
            kind=str(payload.get("kind") or ""),
            source_kind=str(payload.get("source_kind") or ""),
            timestamp=str(payload.get("timestamp") or ""),
            text=str(payload.get("text") or ""),
            tags=list(payload.get("tags") or []),
            model=str(payload.get("model") or ""),
            dimension=int(payload.get("dimension") or (len(vector) if vector else 0)),
            vector=vector,
        )


class ConversationStore:
    def __init__(self, file_path: Path, *, cache_limit: int = 128) -> None:
        self.file_path = file_path
        self.summary_file_path = file_path.with_name(f"{file_path.stem}_summaries.jsonl")
        self.embedding_file_path = file_path.with_name(f"{file_path.stem}_embeddings.jsonl")
        self.file_path.parent.mkdir(parents=True, exist_ok=True)
        self.file_path.touch(exist_ok=True)
        self.summary_file_path.touch(exist_ok=True)
        self.embedding_file_path.touch(exist_ok=True)
        self._lock = threading.Lock()
        self._cache_limit = max(16, cache_limit)
        self._context_cache: OrderedDict[str, str] = OrderedDict()

    def append_turn(self, *, session_id: str, source: str, user_text: str, assistant_text: str) -> None:
        timestamp = datetime.now().astimezone().isoformat()
        self._append_entry(
            ConversationEntry(
                timestamp=timestamp,
                session_id=session_id,
                source=source,
                kind="turn",
                user_text=user_text.strip(),
                assistant_text=assistant_text.strip(),
                entry_id=_stable_document_id("entry", session_id, timestamp, source, "turn", user_text.strip(), assistant_text.strip()),
            )
        )

    def append_note(self, *, session_id: str, source: str, summary: str, tags: list[str] | None = None) -> None:
        timestamp = datetime.now().astimezone().isoformat()
        normalized_summary = summary.strip()
        self._append_entry(
            ConversationEntry(
                timestamp=timestamp,
                session_id=session_id,
                source=source,
                kind="note",
                summary=normalized_summary,
                tags=tags or [],
                entry_id=_stable_document_id("entry", session_id, timestamp, source, "note", normalized_summary),
            )
        )

    def should_use_semantic_rag(self, query: str) -> bool:
        return _needs_long_recall(query)

    def has_embeddings(self, *, session_id: str) -> bool:
        return bool(self._load_embedding_records(session_id=session_id))

    def build_context(
        self,
        *,
        session_id: str,
        query: str,
        query_embedding: list[float] | None = None,
        recent_limit: int = 8,
        retrieval_limit: int = 6,
    ) -> str:
        entries = self._load_entries(session_id=session_id)
        if not entries:
            return ""
        summaries = self._load_summaries(session_id=session_id)
        cache_key = self._make_cache_key(
            session_id=session_id,
            query=query,
            entries=entries,
            summaries=summaries,
            recent_limit=recent_limit,
            retrieval_limit=retrieval_limit,
            has_query_embedding=bool(query_embedding),
        )
        cached = self._cache_get(cache_key)
        if cached is not None:
            return cached

        hot_limit = max(4, recent_limit)
        recent_entries = entries[-hot_limit:]
        older_entries = entries[:-hot_limit] if len(entries) > hot_limit else []
        summary_tail = summaries[-2:]
        summary_candidates = summaries[:-2] if len(summaries) > 2 else []
        long_recall = _needs_long_recall(query)

        lines = [
            f"会话上下文协议: 默认携带最近 {hot_limit} 轮热对话；后台异步维护滚动总结；跨轮指代时再做长期记忆检索。",
        ]
        if summary_tail:
            lines.append("滚动总结:")
            lines.extend(self._format_summaries(summary_tail))
        lines.append("最近交互:")
        lines.extend(self._format_entries(recent_entries))

        retrieved_semantic = []
        if query_embedding:
            retrieved_semantic = self._retrieve_by_embedding(
                session_id=session_id,
                query_embedding=query_embedding,
                entry_candidates=older_entries,
                summary_candidates=summary_candidates,
                limit=min(max(2, retrieval_limit), 5),
            )
        retrieved_lexical = self._retrieve_documents(
            query=query,
            entry_candidates=older_entries,
            summary_candidates=summary_candidates,
            limit=retrieval_limit + (2 if long_recall else 0),
        )
        retrieved = self._merge_retrieved_documents(
            primary=retrieved_semantic,
            secondary=retrieved_lexical,
            limit=retrieval_limit + (1 if long_recall else 0),
        )
        if retrieved:
            lines.append("长期记忆检索:")
            lines.extend(self._format_retrieved_documents(retrieved))

        context = "\n".join(line for line in lines if line.strip())
        self._cache_put(cache_key, context)
        return context

    def get_pending_embedding_documents(self, *, session_id: str, limit: int = 12) -> list[dict[str, Any]]:
        entries = self._load_entries(session_id=session_id)
        summaries = self._load_summaries(session_id=session_id)
        embedded = self._load_embedding_records(session_id=session_id)
        pending: list[dict[str, Any]] = []

        for entry in entries:
            semantic_text = entry.semantic_text()
            if not semantic_text or entry.entry_id in embedded:
                continue
            pending.append(
                {
                    "doc_id": entry.entry_id,
                    "kind": "entry",
                    "source_kind": entry.kind,
                    "timestamp": entry.timestamp,
                    "text": semantic_text,
                    "tags": entry.tags,
                }
            )
        for item in summaries:
            semantic_text = item.semantic_text()
            if not semantic_text or item.summary_id in embedded:
                continue
            pending.append(
                {
                    "doc_id": item.summary_id,
                    "kind": "summary",
                    "source_kind": "summary",
                    "timestamp": item.timestamp,
                    "text": semantic_text,
                    "tags": item.tags,
                }
            )

        pending.sort(key=lambda doc: doc["timestamp"])
        return pending[:limit]

    def save_embeddings(self, *, session_id: str, records: list[dict[str, Any]]) -> int:
        if not records:
            return 0
        by_doc_id = self._load_embedding_records(session_id=session_id)
        for payload in records:
            record = EmbeddingRecord.from_payload(payload)
            if not record.doc_id or not record.vector:
                continue
            by_doc_id[record.doc_id] = record
        self._rewrite_embedding_records(session_id=session_id, records=list(by_doc_id.values()))
        return len(records)

    def recent_entries(self, *, session_id: str, limit: int = 20) -> list[dict[str, Any]]:
        entries = self._load_entries(session_id=session_id)
        return [asdict(item) for item in entries[-max(1, limit) :]]

    def latest_entry(self, *, session_id: str) -> dict[str, Any] | None:
        entries = self._load_entries(session_id=session_id)
        if not entries:
            return None
        return asdict(entries[-1])

    def recent_summaries(self, *, session_id: str, limit: int = 10) -> list[dict[str, Any]]:
        summaries = self._load_summaries(session_id=session_id)
        return [asdict(item) for item in summaries[-max(1, limit) :]]

    def export_context_snapshot(
        self,
        *,
        session_id: str,
        query: str = "",
        recent_limit: int = 8,
        retrieval_limit: int = 6,
        query_embedding: list[float] | None = None,
    ) -> dict[str, Any]:
        return {
            "session_id": session_id,
            "query": query,
            "context_text": self.build_context(
                session_id=session_id,
                query=query,
                query_embedding=query_embedding,
                recent_limit=recent_limit,
                retrieval_limit=retrieval_limit,
            ),
            "recent_entries": self.recent_entries(session_id=session_id, limit=recent_limit),
            "recent_summaries": self.recent_summaries(session_id=session_id, limit=4),
        }

    def retrieve_memory(
        self,
        *,
        session_id: str,
        query: str,
        limit: int = 6,
        query_embedding: list[float] | None = None,
    ) -> list[dict[str, Any]]:
        entries = self._load_entries(session_id=session_id)
        summaries = self._load_summaries(session_id=session_id)
        if not entries and not summaries:
            return []
        entry_candidates = entries[:-max(4, min(12, limit + 2))] if len(entries) > 4 else entries
        summary_candidates = summaries
        semantic_hits = self._retrieve_by_embedding(
            session_id=session_id,
            query_embedding=query_embedding or [],
            entry_candidates=entry_candidates,
            summary_candidates=summary_candidates,
            limit=limit,
        )
        lexical_hits = self._retrieve_documents(
            query=query,
            entry_candidates=entry_candidates,
            summary_candidates=summary_candidates,
            limit=limit,
        )
        return self._merge_retrieved_documents(primary=semantic_hits, secondary=lexical_hits, limit=limit)

    def get_maintenance_payload(
        self,
        *,
        session_id: str,
        hot_turns: int = 8,
        min_turns: int = 6,
        max_turns: int = 12,
    ) -> dict[str, Any] | None:
        entries = self._load_entries(session_id=session_id)
        if len(entries) <= hot_turns + min_turns:
            return None
        summaries = self._load_summaries(session_id=session_id)
        covered_until = max((item.end_index for item in summaries), default=-1)
        pending_end = len(entries) - hot_turns - 1
        if pending_end <= covered_until:
            return None

        start_index = covered_until + 1
        end_index = min(start_index + max_turns - 1, pending_end)
        if end_index - start_index + 1 < min_turns:
            return None

        window = entries[start_index : end_index + 1]
        return {
            "session_id": session_id,
            "start_index": start_index,
            "end_index": end_index,
            "entries": [asdict(entry) for entry in window],
        }

    def store_summary(
        self,
        *,
        session_id: str,
        start_index: int,
        end_index: int,
        summary: str,
        tags: list[str] | None = None,
        source: str = "tool_memory_worker",
    ) -> SummarySnapshot | None:
        if not summary.strip():
            return None
        entries = self._load_entries(session_id=session_id)
        if not entries or start_index < 0 or end_index >= len(entries) or start_index > end_index:
            return None
        timestamp = datetime.now().astimezone().isoformat()
        snapshot = SummarySnapshot(
            timestamp=timestamp,
            session_id=session_id,
            start_index=start_index,
            end_index=end_index,
            start_timestamp=entries[start_index].timestamp,
            end_timestamp=entries[end_index].timestamp,
            summary=summary.strip(),
            tags=tags or [],
            source=source,
            summary_id=_stable_document_id(
                "summary",
                session_id,
                timestamp,
                str(start_index),
                str(end_index),
                summary.strip(),
            ),
        )
        self._append_summary(snapshot)
        return snapshot

    def _append_entry(self, entry: ConversationEntry) -> None:
        line = json.dumps(asdict(entry), ensure_ascii=False)
        with self._lock:
            with self.file_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")
            self._clear_session_cache_locked(entry.session_id)

    def _append_summary(self, snapshot: SummarySnapshot) -> None:
        line = json.dumps(asdict(snapshot), ensure_ascii=False)
        with self._lock:
            with self.summary_file_path.open("a", encoding="utf-8") as handle:
                handle.write(line + "\n")
            self._clear_session_cache_locked(snapshot.session_id)

    def _load_entries(self, *, session_id: str) -> list[ConversationEntry]:
        with self._lock:
            lines = self.file_path.read_text(encoding="utf-8").splitlines()
        entries: list[ConversationEntry] = []
        for line in lines:
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                entry = ConversationEntry.from_payload(payload)
            except Exception:
                continue
            if entry.session_id == session_id:
                entries.append(entry)
        return entries

    def _load_summaries(self, *, session_id: str) -> list[SummarySnapshot]:
        with self._lock:
            lines = self.summary_file_path.read_text(encoding="utf-8").splitlines()
        items: list[SummarySnapshot] = []
        for line in lines:
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                item = SummarySnapshot.from_payload(payload)
            except Exception:
                continue
            if item.session_id == session_id:
                items.append(item)
        return items

    def _load_embedding_records(self, *, session_id: str) -> dict[str, EmbeddingRecord]:
        with self._lock:
            lines = self.embedding_file_path.read_text(encoding="utf-8").splitlines()
        records: dict[str, EmbeddingRecord] = {}
        for line in lines:
            if not line.strip():
                continue
            try:
                payload = json.loads(line)
                record = EmbeddingRecord.from_payload(payload)
            except Exception:
                continue
            if record.session_id != session_id or not record.doc_id:
                continue
            records[record.doc_id] = record
        return records

    def _rewrite_embedding_records(self, *, session_id: str, records: list[EmbeddingRecord]) -> None:
        with self._lock:
            lines = self.embedding_file_path.read_text(encoding="utf-8").splitlines()
            preserved: list[str] = []
            for line in lines:
                if not line.strip():
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if str(payload.get("session_id") or "") != session_id:
                    preserved.append(json.dumps(payload, ensure_ascii=False))
            preserved.extend(json.dumps(asdict(record), ensure_ascii=False) for record in records)
            self.embedding_file_path.write_text("\n".join(preserved) + ("\n" if preserved else ""), encoding="utf-8")
            self._clear_session_cache_locked(session_id)

    def _format_entries(self, entries: list[ConversationEntry]) -> list[str]:
        lines: list[str] = []
        for entry in entries:
            if entry.kind == "turn":
                lines.append(f"- [{entry.timestamp}] 用户: {entry.user_text}")
                lines.append(f"  助手: {entry.assistant_text}")
            else:
                tag_text = f" tags={','.join(entry.tags)}" if entry.tags else ""
                lines.append(f"- [{entry.timestamp}] 观察{tag_text}: {entry.summary}")
        return lines

    def _format_summaries(self, summaries: list[SummarySnapshot]) -> list[str]:
        lines: list[str] = []
        for item in summaries:
            tag_text = f" tags={','.join(item.tags)}" if item.tags else ""
            lines.append(
                f"- [{item.start_timestamp} ~ {item.end_timestamp}] 摘要{tag_text}: {item.summary}"
            )
        return lines

    def _format_retrieved_documents(self, documents: list[dict[str, Any]]) -> list[str]:
        lines: list[str] = []
        for item in documents:
            if item["type"] == "entry":
                entry: ConversationEntry = item["payload"]
                if entry.kind == "turn":
                    lines.append(f"- [{entry.timestamp}] 相关问答: 用户说“{entry.user_text}” / 助手答“{entry.assistant_text}”")
                else:
                    lines.append(f"- [{entry.timestamp}] 相关观察: {entry.summary}")
            else:
                snapshot: SummarySnapshot = item["payload"]
                lines.append(
                    f"- [{snapshot.start_timestamp} ~ {snapshot.end_timestamp}] 相关阶段摘要: {snapshot.summary}"
                )
        return lines

    def _retrieve_documents(
        self,
        *,
        query: str,
        entry_candidates: list[ConversationEntry],
        summary_candidates: list[SummarySnapshot],
        limit: int,
    ) -> list[dict[str, Any]]:
        if not query.strip():
            return []
        query_vec = _vectorize_text(query)
        if not query_vec:
            return []
        scored: list[tuple[float, dict[str, Any]]] = []
        for entry in entry_candidates:
            text = entry.searchable_text()
            if not text:
                continue
            score = _cosine_similarity_sparse(query_vec, _vectorize_text(text))
            if entry.kind == "note":
                score += 0.04
            if _needs_long_recall(query):
                score += 0.08
            if score >= 0.18:
                scored.append((score, {"type": "entry", "payload": entry}))
        for item in summary_candidates:
            text = item.searchable_text()
            if not text:
                continue
            score = _cosine_similarity_sparse(query_vec, _vectorize_text(text)) + 0.05
            if _needs_long_recall(query):
                score += 0.08
            if score >= 0.16:
                scored.append((score, {"type": "summary", "payload": item}))
        scored.sort(key=lambda pair: pair[0], reverse=True)
        return [payload for _, payload in scored[:limit]]

    def _retrieve_by_embedding(
        self,
        *,
        session_id: str,
        query_embedding: list[float],
        entry_candidates: list[ConversationEntry],
        summary_candidates: list[SummarySnapshot],
        limit: int,
    ) -> list[dict[str, Any]]:
        if not query_embedding:
            return []
        embedding_records = self._load_embedding_records(session_id=session_id)
        if not embedding_records:
            return []

        candidates: list[tuple[str, dict[str, Any]]] = []
        for entry in entry_candidates:
            candidates.append((entry.entry_id, {"type": "entry", "payload": entry}))
        for item in summary_candidates:
            candidates.append((item.summary_id, {"type": "summary", "payload": item}))

        scored: list[tuple[float, dict[str, Any]]] = []
        for doc_id, payload in candidates:
            record = embedding_records.get(doc_id)
            if record is None or not record.vector:
                continue
            score = _cosine_similarity_dense(query_embedding, record.vector)
            if payload["type"] == "summary":
                score += 0.03
            if score >= 0.08:
                scored.append((score, payload))
        scored.sort(key=lambda pair: pair[0], reverse=True)
        return [payload for _, payload in scored[:limit]]

    def _merge_retrieved_documents(
        self,
        *,
        primary: list[dict[str, Any]],
        secondary: list[dict[str, Any]],
        limit: int,
    ) -> list[dict[str, Any]]:
        merged: list[dict[str, Any]] = []
        seen: set[str] = set()
        for batch in (primary, secondary):
            for item in batch:
                payload = item["payload"]
                doc_id = payload.entry_id if item["type"] == "entry" else payload.summary_id
                if doc_id in seen:
                    continue
                seen.add(doc_id)
                merged.append(item)
                if len(merged) >= limit:
                    return merged
        return merged

    def _make_cache_key(
        self,
        *,
        session_id: str,
        query: str,
        entries: list[ConversationEntry],
        summaries: list[SummarySnapshot],
        recent_limit: int,
        retrieval_limit: int,
        has_query_embedding: bool,
    ) -> str:
        entry_tail = entries[-1].timestamp if entries else "none"
        summary_tail = summaries[-1].timestamp if summaries else "none"
        normalized_query = " ".join(query.lower().split())
        return "|".join(
            [
                session_id,
                normalized_query,
                str(recent_limit),
                str(retrieval_limit),
                "semantic" if has_query_embedding else "lexical",
                str(len(entries)),
                entry_tail,
                str(len(summaries)),
                summary_tail,
            ]
        )

    def _cache_get(self, key: str) -> str | None:
        with self._lock:
            cached = self._context_cache.get(key)
            if cached is None:
                return None
            self._context_cache.move_to_end(key)
            return cached

    def _cache_put(self, key: str, value: str) -> None:
        with self._lock:
            self._context_cache[key] = value
            self._context_cache.move_to_end(key)
            while len(self._context_cache) > self._cache_limit:
                self._context_cache.popitem(last=False)

    def _clear_session_cache_locked(self, session_id: str) -> None:
        stale_keys = [key for key in self._context_cache.keys() if key.startswith(f"{session_id}|")]
        for key in stale_keys:
            self._context_cache.pop(key, None)


def _needs_long_recall(query: str) -> bool:
    lowered = query.lower()
    markers = (
        "刚才",
        "刚刚",
        "前面",
        "之前",
        "上次",
        "继续",
        "那个",
        "这个",
        "那张",
        "这张",
        "图片上的",
        "图里",
        "他",
        "她",
        "它",
        "reply",
        "上文",
        "还记得",
        "你刚说",
    )
    return any(marker in lowered for marker in markers)


def _vectorize_text(text: str) -> dict[int, float]:
    tokens: list[str] = []
    lowered = text.lower()
    tokens.extend(match.group(0) for match in _TOKEN_RE.finditer(lowered))
    for match in _CJK_RE.finditer(text):
        chars = match.group(0)
        tokens.extend(chars[i : i + 2] for i in range(max(1, len(chars) - 1)))
    vector: dict[int, float] = {}
    for token in tokens:
        slot = hash(token) % 768
        vector[slot] = vector.get(slot, 0.0) + 1.0
    return vector


def _cosine_similarity_sparse(left: dict[int, float], right: dict[int, float]) -> float:
    if not left or not right:
        return 0.0
    dot = sum(value * right.get(key, 0.0) for key, value in left.items())
    left_norm = math.sqrt(sum(value * value for value in left.values()))
    right_norm = math.sqrt(sum(value * value for value in right.values()))
    if left_norm == 0.0 or right_norm == 0.0:
        return 0.0
    return dot / (left_norm * right_norm)


def _cosine_similarity_dense(left: list[float], right: list[float]) -> float:
    if not left or not right or len(left) != len(right):
        return 0.0
    dot = sum(a * b for a, b in zip(left, right))
    left_norm = math.sqrt(sum(value * value for value in left))
    right_norm = math.sqrt(sum(value * value for value in right))
    if left_norm == 0.0 or right_norm == 0.0:
        return 0.0
    return dot / (left_norm * right_norm)
