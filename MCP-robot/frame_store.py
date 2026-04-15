from __future__ import annotations

import json
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


@dataclass(slots=True)
class FrameRecord:
    frame_id: str
    timestamp: str
    device_id: str
    mime_type: str
    image_base64: str
    note: str = ""
    lowres_summary: str = ""


class FrameStore:
    def __init__(self, root_dir: Path) -> None:
        self.root_dir = root_dir
        self.root_dir.mkdir(parents=True, exist_ok=True)

    def save_frame(
        self,
        *,
        device_id: str,
        mime_type: str,
        image_base64: str,
        note: str = "",
    ) -> FrameRecord:
        frame = FrameRecord(
            frame_id=uuid.uuid4().hex[:16],
            timestamp=datetime.now().astimezone().isoformat(),
            device_id=device_id,
            mime_type=mime_type,
            image_base64=image_base64,
            note=note,
        )
        self._write(frame)
        self._write_latest_pointer(frame.frame_id)
        return frame

    def update_summary(self, frame_id: str, summary: str) -> None:
        frame = self.load(frame_id)
        frame.lowres_summary = summary
        self._write(frame)

    def load(self, frame_id: str) -> FrameRecord:
        path = self.root_dir / f"{frame_id}.json"
        payload = json.loads(path.read_text(encoding="utf-8"))
        return FrameRecord(**payload)

    def latest(self) -> FrameRecord | None:
        pointer = self.root_dir / "latest.txt"
        if not pointer.exists():
            return None
        frame_id = pointer.read_text(encoding="utf-8").strip()
        if not frame_id:
            return None
        path = self.root_dir / f"{frame_id}.json"
        if not path.exists():
            return None
        return self.load(frame_id)

    def summary_context(self) -> str:
        frame = self.latest()
        if frame is None:
            return ""
        summary = frame.lowres_summary or frame.note or "最近收到过图像帧，但还没有视觉摘要。"
        return f"最近图像帧: id={frame.frame_id} time={frame.timestamp} summary={summary}"

    def _write(self, frame: FrameRecord) -> None:
        path = self.root_dir / f"{frame.frame_id}.json"
        path.write_text(json.dumps(asdict(frame), ensure_ascii=False), encoding="utf-8")

    def _write_latest_pointer(self, frame_id: str) -> None:
        (self.root_dir / "latest.txt").write_text(frame_id, encoding="utf-8")
