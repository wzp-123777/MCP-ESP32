from __future__ import annotations

import json
from typing import Any

from tools.base import ToolResult


def _tool_result_payload(result: ToolResult) -> dict[str, Any]:
    return {
        "tool_name": result.tool_name,
        "ok": result.ok,
        "content": result.content,
        "device_payload": result.device_payload,
        "error": result.error,
    }


def build_mcp_facade(runtime):
    try:
        from mcp.server.fastmcp import FastMCP
    except Exception:
        return None

    app = FastMCP(
        name="MCP Robot Facade",
        instructions=(
            "这是 MCP-Robot 的标准 MCP facade。"
            "它把现有运行时能力暴露成 tools/resources/prompts，方便外部 agent 读取上下文、记忆、图片状态并调用工具。"
        ),
    )

    @app.tool(name="tool_catalog", description="列出当前工具能力清单与元数据。")
    async def tool_catalog() -> dict[str, Any]:
        return {"tools": runtime.tool_registry.catalog()}

    @app.tool(name="execute_registered_tool", description="执行 MCP-Robot 内部已注册工具。")
    async def execute_registered_tool(name: str, arguments_json: str = "{}") -> dict[str, Any]:
        tool = runtime.tool_registry.get(name)
        if tool is None:
            return {"ok": False, "error": f"unknown tool: {name}"}
        try:
            arguments = json.loads(arguments_json or "{}")
        except json.JSONDecodeError as exc:
            return {"ok": False, "error": f"bad arguments json: {exc}"}
        if tool.should_require_approval(arguments):
            capability = tool.capability()
            return {
                "ok": False,
                "approval_required": True,
                "tool": capability.name,
                "risk_level": capability.risk_level,
                "can_direct_device": capability.can_direct_device,
            }
        result = await tool.execute(arguments)
        return _tool_result_payload(result)

    @app.tool(name="query_long_term_memory", description="按查询语句检索共享长期记忆。")
    async def query_long_term_memory(query: str, limit: int = 6) -> dict[str, Any]:
        return await runtime.long_term_memory_snapshot(query=query, limit=max(1, min(limit, 12)))

    @app.tool(name="list_recent_tasks", description="查看后台任务最近状态。")
    async def list_recent_tasks(limit: int = 20) -> dict[str, Any]:
        return {"tasks": await runtime.recent_task_snapshot(limit=max(1, min(limit, 50)))}

    @app.tool(name="list_pending_approvals", description="查看当前待审批的高危操作。")
    async def list_pending_approvals(source: str = "", user_id: str = "") -> dict[str, Any]:
        return {"approvals": await runtime.pending_approval_snapshot(source=source or None, user_id=user_id)}

    @app.tool(name="submit_root_command", description="提交 root agent 项目级命令；默认只返回审批需求。")
    async def submit_root_command(command: str, auto_approve: bool = False) -> dict[str, Any]:
        if not command.strip():
            return {"ok": False, "error": "command is required"}
        if runtime.config.high_risk_approval_enabled and not auto_approve:
            return {
                "ok": False,
                "approval_required": True,
                "action_type": "root_command",
                "summary": f"root agent 执行项目级命令: {command.strip()}",
            }
        bridge_prompt = (
            f"你正在作为 root agent 处理来自 MCP facade 的项目级命令。\n"
            f"主项目根目录: {runtime.config.project_root}\n"
            f"GenericAgent 目录: {runtime.config.generic_agent_root}\n"
            "优先处理主项目根目录里的文件，跨目录操作时请明确说明。\n"
            "只在任务真正完成后再给出结果，不要把中间计划、工具调用或脚本草稿当最终答复。\n"
            f"用户命令: {command.strip()}"
        )
        raw_result = await runtime.agent_bridge.run_root_command(bridge_prompt)
        result = raw_result
        if runtime._should_summarize_root_result(raw_result):
            result = await runtime.language_model.summarize_agent_result(command=command.strip(), raw_result=raw_result, max_chars=900)
        return {"ok": True, "result": result, "raw_result": raw_result}

    @app.resource("mcp://runtime/health", name="runtime_health", description="运行时健康快照。", mime_type="application/json")
    def runtime_health() -> str:
        return json.dumps(runtime.health_snapshot(), ensure_ascii=False, indent=2)

    @app.resource("mcp://context/shared-window", name="shared_context_window", description="共享上下文窗口。", mime_type="application/json")
    def shared_context_window() -> str:
        return json.dumps(runtime.shared_context_snapshot(), ensure_ascii=False, indent=2)

    @app.resource("mcp://memory/subconscious", name="subconscious_memory", description="潜意识记忆快照。", mime_type="application/json")
    def subconscious_memory() -> str:
        return json.dumps(runtime.subconscious_snapshot(limit=12), ensure_ascii=False, indent=2)

    @app.resource("mcp://frames/latest", name="latest_frame", description="最近一张图片帧。", mime_type="application/json")
    def latest_frame() -> str:
        return json.dumps(runtime.latest_frame_snapshot() or {}, ensure_ascii=False, indent=2)

    @app.resource("mcp://tasks/recent", name="recent_tasks", description="最近后台任务。", mime_type="application/json")
    async def recent_tasks() -> str:
        return json.dumps({"tasks": await runtime.recent_task_snapshot(limit=20)}, ensure_ascii=False, indent=2)

    @app.prompt(name="chat_with_current_context", description="让外部 agent 用当前共享上下文继续对话。")
    def chat_with_current_context(user_text: str) -> str:
        context = runtime.shared_context_snapshot()
        return (
            "请先阅读下面的共享上下文，再回复新的用户输入。\n\n"
            f"{json.dumps(context, ensure_ascii=False, indent=2)}\n\n"
            f"用户输入: {user_text}"
        )

    @app.prompt(name="inspect_latest_image", description="围绕最近一张图片继续推理。")
    def inspect_latest_image(question: str) -> str:
        latest = runtime.latest_frame_snapshot() or {}
        return (
            "请根据最近图片帧元数据与共享上下文回答问题；如果信息不足，要明确说明。\n\n"
            f"最近图片帧:\n{json.dumps(latest, ensure_ascii=False, indent=2)}\n\n"
            f"问题: {question}"
        )

    @app.prompt(name="operate_root_agent_safely", description="给外部 agent 一个安全的 root 操作模板。")
    def operate_root_agent_safely(command: str) -> str:
        return (
            "这是一个高风险 root 操作，请先确认边界，再执行。\n"
            "要求：只在项目目录内操作；不要返回中间计划；最终只给出结果。\n"
            f"命令: {command}"
        )

    return app
