from __future__ import annotations

import asyncio
import time
import uuid
from dataclasses import asdict, dataclass, field
from typing import Any, Awaitable, Callable


@dataclass(slots=True)
class ApprovalRequest:
    approval_id: str
    action_type: str
    summary: str
    source: str
    user_id: str = ""
    session_id: str = ""
    created_at: float = field(default_factory=time.time)
    expires_at: float = 0.0
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class ApprovalManager:
    def __init__(self, *, ttl_seconds: float = 300.0) -> None:
        self.ttl_seconds = max(60.0, ttl_seconds)
        self._pending: dict[str, tuple[ApprovalRequest, Callable[[], Awaitable[Any]]]] = {}
        self._lock = asyncio.Lock()

    async def create(
        self,
        *,
        action_type: str,
        summary: str,
        source: str,
        user_id: str = "",
        session_id: str = "",
        metadata: dict[str, Any] | None = None,
        executor: Callable[[], Awaitable[Any]],
    ) -> ApprovalRequest:
        approval = ApprovalRequest(
            approval_id=uuid.uuid4().hex[:8],
            action_type=action_type,
            summary=summary,
            source=source,
            user_id=user_id,
            session_id=session_id,
            expires_at=time.time() + self.ttl_seconds,
            metadata=dict(metadata or {}),
        )
        async with self._lock:
            self._cleanup_locked()
            self._pending[approval.approval_id] = (approval, executor)
        return approval

    async def approve(self, approval_id: str, *, source: str, user_id: str = "") -> tuple[ApprovalRequest | None, Callable[[], Awaitable[Any]] | None]:
        async with self._lock:
            self._cleanup_locked()
            item = self._pending.get(approval_id)
            if item is None:
                return None, None
            approval, executor = item
            if approval.source != source:
                return None, None
            if approval.user_id and user_id and approval.user_id != user_id:
                return None, None
            self._pending.pop(approval_id, None)
            return approval, executor

    async def reject(self, approval_id: str, *, source: str, user_id: str = "") -> ApprovalRequest | None:
        async with self._lock:
            self._cleanup_locked()
            item = self._pending.get(approval_id)
            if item is None:
                return None
            approval, _ = item
            if approval.source != source:
                return None
            if approval.user_id and user_id and approval.user_id != user_id:
                return None
            self._pending.pop(approval_id, None)
            return approval

    async def list_pending(self, *, source: str | None = None, user_id: str = "") -> list[dict[str, Any]]:
        async with self._lock:
            self._cleanup_locked()
            records: list[dict[str, Any]] = []
            for approval, _ in self._pending.values():
                if source and approval.source != source:
                    continue
                if user_id and approval.user_id and approval.user_id != user_id:
                    continue
                records.append(approval.to_dict())
            records.sort(key=lambda item: item["created_at"], reverse=True)
            return records

    def _cleanup_locked(self) -> None:
        now = time.time()
        expired = [approval_id for approval_id, (approval, _) in self._pending.items() if approval.expires_at <= now]
        for approval_id in expired:
            self._pending.pop(approval_id, None)

