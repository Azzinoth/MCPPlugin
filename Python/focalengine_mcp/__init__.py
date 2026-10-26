"""MCP server of MCPPlugin, AI assistants use running Focal Engine application through its tools.

Server runs on its own thread, engine is used only on main thread through focalengine.call_on_main_thread.
"""
from .server import get_url, is_running, start, stop

__all__ = ["start", "stop", "is_running", "get_url"]