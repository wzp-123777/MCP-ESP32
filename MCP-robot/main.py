from __future__ import annotations

import json
import logging
from logging.handlers import RotatingFileHandler
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

from app_config import load_config
from runtime import RobotRuntime


config = load_config()
config.data_dir.mkdir(parents=True, exist_ok=True)

log_format = "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
console_handler = logging.StreamHandler()
console_handler.setFormatter(logging.Formatter(log_format))
runtime_file_handler = RotatingFileHandler(
    config.runtime_log_file,
    maxBytes=2 * 1024 * 1024,
    backupCount=3,
    encoding="utf-8",
)
runtime_file_handler.setFormatter(logging.Formatter(log_format))

root_logger = logging.getLogger()
root_logger.setLevel(logging.INFO)
root_logger.handlers.clear()
root_logger.addHandler(console_handler)
root_logger.addHandler(runtime_file_handler)

trace_handler = RotatingFileHandler(
    config.trace_log_file,
    maxBytes=4 * 1024 * 1024,
    backupCount=5,
    encoding="utf-8",
)
trace_handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
trace_console_handler = logging.StreamHandler()
trace_console_handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
trace_logger = logging.getLogger("MCP_Robot.Trace")
trace_logger.setLevel(logging.INFO)
trace_logger.handlers.clear()
trace_logger.addHandler(trace_handler)
trace_logger.addHandler(trace_console_handler)
trace_logger.propagate = False

logger = logging.getLogger("MCP_Robot_Brain")

app = FastAPI(title="云龙虾 (Cloud Lobster) 并行异构控制脑")


class ConnectionManager:
    def __init__(self) -> None:
        self.qq_client: WebSocket | None = None
        self.esp32_client: WebSocket | None = None

    @property
    def is_esp32_connected(self) -> bool:
        return self.esp32_client is not None

    async def connect_qq(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self.qq_client = websocket
        logger.info("QQ 终端 (NapCat) 已连接。")

    async def connect_esp32(self, websocket: WebSocket) -> None:
        await websocket.accept()
        self.esp32_client = websocket
        logger.info("ESP32 终端已连接。")

    def disconnect_qq(self) -> None:
        self.qq_client = None
        logger.warning("QQ 终端断开连接。")

    def disconnect_esp32(self) -> None:
        self.esp32_client = None
        logger.warning("ESP32 终端断开连接。")

    async def send_to_qq(self, message: dict[str, Any]) -> None:
        if self.qq_client is None:
            logger.warning("QQ 终端未连接，消息未送达。")
            return
        await self.qq_client.send_text(json.dumps(message, ensure_ascii=False))

    async def send_to_esp32(self, message: dict[str, Any]) -> None:
        if self.esp32_client is None:
            logger.debug("ESP32 未连接，忽略下行消息: %s", message.get("type"))
            return
        await self.esp32_client.send_text(json.dumps(message, ensure_ascii=False))

manager = ConnectionManager()
runtime = RobotRuntime(config, manager)


@app.websocket("/ws")
async def qq_endpoint(websocket: WebSocket) -> None:
    await manager.connect_qq(websocket)
    try:
        while True:
            payload = json.loads(await websocket.receive_text())
            await runtime.handle_napcat_payload(payload)
    except WebSocketDisconnect:
        manager.disconnect_qq()
    except Exception as exc:
        logger.exception("QQ 通道异常: %s", exc)
        manager.disconnect_qq()


@app.websocket("/esp32_ws")
async def esp32_endpoint(websocket: WebSocket) -> None:
    await manager.connect_esp32(websocket)
    try:
        while True:
            payload = json.loads(await websocket.receive_text())
            await runtime.handle_esp32_payload(payload)
    except WebSocketDisconnect:
        manager.disconnect_esp32()
    except Exception as exc:
        logger.exception("ESP32 通道异常: %s", exc)
        manager.disconnect_esp32()


if __name__ == "__main__":
    import uvicorn

    logger.info("启动并行异构控制脑，端口 8080。")
    logger.info("运行日志: %s", config.runtime_log_file)
    logger.info("模型轨迹/聊天记录日志: %s", config.trace_log_file)
    logger.info(
        "模型路由: language=%s | tool=%s | vision_low=%s | vision_high=%s | tts=%s",
        config.language_model.model,
        config.tool_model.model,
        config.vision_model.model,
        config.vision_highres_model.model,
        config.tts_model.model,
    )
    logger.info("GenericAgent Python: %s", config.generic_agent_python)
    uvicorn.run(app, host="0.0.0.0", port=8080)
