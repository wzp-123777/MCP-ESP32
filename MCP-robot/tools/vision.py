from __future__ import annotations

from typing import Any

from frame_store import FrameStore
from models import VisionModelService
from tools.base import BaseTool, ToolResult


class HighResVisionTool(BaseTool):
    name = "vision.inspect_highres"
    description = "对最近一张或指定 frame_id 的图片做高清细节检查。"
    output_schema = {
        "type": "object",
        "properties": {
            "frame_id": {"type": "string"},
            "timestamp": {"type": "string"},
            "answer": {"type": "string"},
            "confidence": {"type": ["number", "null"]},
        },
    }
    background_capable = True
    tags = ("vision", "image", "inspection")
    input_schema = {
        "type": "object",
        "properties": {
            "question": {"type": "string", "description": "你想检查图片里的什么内容"},
            "frame_id": {"type": "string", "description": "可选，指定图像帧 ID；为空时使用最近一张"},
        },
        "required": ["question"],
    }

    def __init__(self, frame_store: FrameStore, vision_service: VisionModelService) -> None:
        self.frame_store = frame_store
        self.vision_service = vision_service

    async def execute(self, arguments: dict[str, Any]) -> ToolResult:
        question = str(arguments.get("question") or "").strip()
        if not question:
            return ToolResult(self.name, False, None, error="question is required")
        frame_id = str(arguments.get("frame_id") or "").strip()
        frame = self.frame_store.load(frame_id) if frame_id else self.frame_store.latest()
        if frame is None:
            return ToolResult(self.name, False, None, error="no cached frame available")
        result = await self.vision_service.inspect_high_res(
            image_base64=frame.image_base64,
            mime_type=frame.mime_type,
            question=question,
            scene_summary=frame.lowres_summary,
        )
        return ToolResult(
            self.name,
            True,
            {
                "frame_id": frame.frame_id,
                "timestamp": frame.timestamp,
                **result,
            },
        )
