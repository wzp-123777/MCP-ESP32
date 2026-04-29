from __future__ import annotations

import asyncio
import logging
import time
import uuid
from dataclasses import asdict, dataclass, field
from typing import Any, Awaitable, Callable


logger = logging.getLogger("MCP_Robot.TaskManager")


@dataclass(slots=True)
class TaskRecord:
    task_id: str
    task_type: str
    status: str = "queued"
    progress: float = 0.0
    message: str = ""
    source: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)
    timeout_seconds: float | None = None
    retries: int = 0
    attempt: int = 0
    result: Any = None
    error: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class BackgroundTaskManager:
    def __init__(self, *, max_history: int = 256) -> None:
        self.max_history = max(32, max_history)
        self._records: dict[str, TaskRecord] = {}
        self._order: list[str] = []
        self._lock = asyncio.Lock()

    async def create_task(
        self,
        *,
        task_type: str,
        coro_factory: Callable[[str], Awaitable[Any]],
        source: str = "",
        metadata: dict[str, Any] | None = None,
        timeout_seconds: float | None = None,
        retries: int = 0,
    ) -> tuple[str, asyncio.Task[Any]]:
        record = TaskRecord(
            task_id=uuid.uuid4().hex[:12],
            task_type=task_type,
            source=source,
            metadata=dict(metadata or {}),
            timeout_seconds=timeout_seconds,
            retries=max(0, retries),
        )
        async with self._lock:
            self._records[record.task_id] = record
            self._order.append(record.task_id)
            self._trim_locked()
        task = asyncio.create_task(self._runner(record.task_id, coro_factory))
        return record.task_id, task

    async def update_progress(self, task_id: str, *, progress: float, message: str = "") -> None:
        async with self._lock:
            record = self._records.get(task_id)
            if record is None:
                return
            record.progress = max(0.0, min(1.0, progress))
            if message:
                record.message = message
            record.updated_at = time.time()

    async def snapshot(self, *, limit: int = 20) -> list[dict[str, Any]]:
        async with self._lock:
            task_ids = self._order[-max(1, limit) :]
            return [self._records[task_id].to_dict() for task_id in reversed(task_ids) if task_id in self._records]

    async def get(self, task_id: str) -> dict[str, Any] | None:
        async with self._lock:
            record = self._records.get(task_id)
            return record.to_dict() if record else None

    async def _runner(self, task_id: str, coro_factory: Callable[[str], Awaitable[Any]]) -> Any:
        while True:
            record = await self._require_record(task_id)
            if record is None:
                return None
            record.status = "running"
            record.attempt += 1
            record.updated_at = time.time()
            try:
                if record.timeout_seconds:
                    result = await asyncio.wait_for(coro_factory(task_id), timeout=record.timeout_seconds)
                else:
                    result = await coro_factory(task_id)
            except asyncio.CancelledError:
                await self._finish(task_id, status="cancelled", error="cancelled")
                raise
            except Exception as exc:
                if record.attempt <= record.retries:
                    await self._retry(task_id, str(exc))
                    continue
                await self._finish(task_id, status="failed", error=str(exc))
                raise
            await self._finish(task_id, status="completed", result=result)
            return result

    async def _retry(self, task_id: str, error: str) -> None:
        async with self._lock:
            record = self._records.get(task_id)
            if record is None:
                return
            record.status = "retrying"
            record.error = error
            record.message = f"retrying after error: {error}"
            record.updated_at = time.time()

    async def _finish(self, task_id: str, *, status: str, result: Any = None, error: str = "") -> None:
        async with self._lock:
            record = self._records.get(task_id)
            if record is None:
                return
            record.status = status
            record.progress = 1.0 if status == "completed" else record.progress
            record.result = result
            record.error = error
            record.updated_at = time.time()

    async def _require_record(self, task_id: str) -> TaskRecord | None:
        async with self._lock:
            return self._records.get(task_id)

    def _trim_locked(self) -> None:
        while len(self._order) > self.max_history:
            task_id = self._order.pop(0)
            self._records.pop(task_id, None)

