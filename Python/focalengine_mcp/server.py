"""MCP streamable HTTP server on its own thread, it never uses engine directly, only through calls on main thread."""
import asyncio
import json
import logging
import os
import queue
import site
import socket
import sys
import threading

import focalengine

from . import tools

HOST = "127.0.0.1"
SERVER_NAME = "focalengine"
SERVER_VERSION = "1.0.0"
INSTRUCTIONS = (
    "Tools of a running Focal Engine application. Y axis is up. Positions are absolute. "
    "Rotations are Euler angles in degrees, applied around X, then Y, then Z. "
    "In the editor, play mode runs on a copy of the scene with GameMode flag, its scene and entity IDs differ from the edited scene. "
    "World space values are recomputed later in the frame, so reading them right after a change could return old values."
)
# Main thread could be blocked, for example by a modal dialog, call that did not start in time is cancelled.
QUEUED_CALL_TIMEOUT_IN_SECONDS = 15.0
GRACEFUL_SHUTDOWN_TIMEOUT_IN_SECONDS = 2
# Engine log is written only on main thread, so records of these loggers are queued and written there.
LOGGER_NAMES = ("focalengine_mcp", "mcp", "uvicorn", "asyncio", "sse_starlette")
# Some packages could not be loaded again after Python was finalized and initialized in the same process.
IMPORT_MARKER = "FOCAL_ENGINE_MCP_PACKAGES_IMPORTED"
# Native functions that MCPPlugin registers in PythonPlugin, through them server serves tools that applications and plugins registered from C++.
LIST_NATIVE_TOOLS_FUNCTION = "MCPPlugin.ListTools"
CALL_NATIVE_TOOL_FUNCTION = "MCPPlugin.CallTool"
SET_RESERVED_TOOL_NAMES_FUNCTION = "MCPPlugin.SetReservedToolNames"

logger = logging.getLogger("focalengine_mcp")
_server = None


class _MainThreadLogHandler(logging.Handler):
    def __init__(self):
        super().__init__()
        self.setFormatter(logging.Formatter("%(name)s: %(message)s"))
        self.records = queue.SimpleQueue()
        self.write_scheduled = threading.Event()

    def emit(self, record):
        try:
            self.records.put((record.levelno, self.format(record)))
            if focalengine.is_main_thread():
                self.write_to_engine_log()
            elif not self.write_scheduled.is_set():
                self.write_scheduled.set()
                focalengine.call_on_main_thread(self.write_to_engine_log)
        except Exception:
            # PythonPlugin is unloading, records stay queued until stop() writes them.
            pass

    def write_to_engine_log(self):
        self.write_scheduled.clear()
        while True:
            try:
                level, text = self.records.get_nowait()
            except queue.Empty:
                return

            if level >= logging.ERROR:
                focalengine.log(text, "error")
            elif level >= logging.WARNING:
                focalengine.log(text, "warning")
            elif level >= logging.INFO:
                focalengine.log(text, "info")
            else:
                focalengine.log(text, "debug")


def _get_packages_directory():
    # Installed plugin keeps packages in Data/PythonPackages next to Data/Python, source folder of plugin keeps them in ThirdParty/PythonPackages.
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    for directory in (os.path.join(root, "PythonPackages"), os.path.join(root, "ThirdParty", "PythonPackages")):
        if os.path.isdir(directory):
            return directory

    raise RuntimeError(f"Python packages of MCP server are not found next to {root}")


def _bind_socket(port):
    listening_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Without it Windows lets other process bind the same port.
    listening_socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    try:
        listening_socket.bind((HOST, port))
    except OSError as error:
        listening_socket.close()
        if getattr(error, "winerror", None) == 10048:
            raise RuntimeError(f"Port {port} is already in use, for example by other running application with MCP server, "
                               "other port could be set with FOCAL_ENGINE_MCP_PORT environment variable") from None

        if getattr(error, "winerror", None) == 10013:
            raise RuntimeError(f"Port {port} could not be used, it is probably in a range that Windows reserved "
                               "(netsh interface ipv4 show excludedportrange protocol=tcp), "
                               "other port could be set with FOCAL_ENGINE_MCP_PORT environment variable") from None

        raise

    return listening_socket


def _make_native_tool_handler(name):
    def handle(arguments):
        # Arguments stay text, so C++ handler gets numbers exactly as client sent them.
        output = json.loads(focalengine.call_native_function(CALL_NATIVE_TOOL_FUNCTION, json.dumps({"name": name, "arguments_json": json.dumps(arguments)})))
        return tools.ToolOutput(output.get("text", ""), output.get("is_error", False), output.get("image_base64", ""), output.get("image_mime_type", ""))

    return handle


class _EngineServer:
    def __init__(self, port):
        self.port = port
        self.url = f"http://{HOST}:{port}/mcp"
        # Replaced as a whole when tools change, so server thread always reads complete dictionary.
        self.tools = {tool.name: tool for tool in tools.BUILT_IN_TOOLS}
        self.socket = None
        self.thread = None
        self.loop = None
        self.uvicorn_server = None
        self.listen_handler = None
        self.subscription_bus = None
        # Sessions of handshake-era clients, they get tools/list_changed directly instead of through subscriptions/listen.
        # Session object is created for every request, so the last one of each session ID is kept, sessions that could not be reached are removed.
        self.sessions = {}
        self.pending_futures = set()
        self.pending_futures_lock = threading.Lock()
        self.stopping = threading.Event()
        self.shutdown_callback_id = None
        self.log_handler = None
        self.previous_propagate_values = {}

    def start(self):
        self.socket = _bind_socket(self.port)
        try:
            self.log_handler = _MainThreadLogHandler()
            self.log_handler.addFilter(self._filter_stopping_noise)
            for name in LOGGER_NAMES:
                package_logger = logging.getLogger(name)
                self.previous_propagate_values[name] = package_logger.propagate
                package_logger.propagate = False
                package_logger.addHandler(self.log_handler)
            logger.setLevel(logging.INFO)

            # Backup, MCPPlugin stops server itself before PythonPlugin is unloaded.
            self.shutdown_callback_id = focalengine.add_shutdown_callback(self.stop)

            os.environ[IMPORT_MARKER] = "1"
            self.thread = threading.Thread(target=self._run, name="MCP server")
            self.thread.start()
        except BaseException:
            self.socket.close()
            self._detach_log_handler()
            if self.shutdown_callback_id is not None:
                focalengine.remove_callback(self.shutdown_callback_id)
                self.shutdown_callback_id = None
            self.thread = None
            raise

    def load_native_tools(self):
        """Runs on main thread, adds tools registered from C++ to built-in tools."""
        # PythonPlugin before interface version 2 has no native functions.
        if not hasattr(focalengine, "get_native_function_names") or LIST_NATIVE_TOOLS_FUNCTION not in focalengine.get_native_function_names():
            return

        built_in_tools = {tool.name: tool for tool in tools.BUILT_IN_TOOLS}
        focalengine.call_native_function(SET_RESERVED_TOOL_NAMES_FUNCTION, json.dumps(sorted(built_in_tools)))
        all_tools = dict(built_in_tools)
        for description in json.loads(focalengine.call_native_function(LIST_NATIVE_TOOLS_FUNCTION)):
            name = description["name"]
            if name in built_in_tools:
                logger.warning("Tool %s registered from C++ is not served, because built-in tool has the same name", name)
                continue

            all_tools[name] = tools.Tool(name, description.get("description", ""), description["input_schema"], _make_native_tool_handler(name))
        self.tools = all_tools

    def _filter_stopping_noise(self, record):
        # uvicorn reports open SSE streams that stop() ends as unfinished responses.
        return not (self.stopping.is_set() and record.getMessage() == "ASGI callable returned without completing response.")

    def _detach_log_handler(self):
        if self.log_handler is None:
            return

        for name in self.previous_propagate_values:
            package_logger = logging.getLogger(name)
            package_logger.removeHandler(self.log_handler)
            package_logger.propagate = self.previous_propagate_values[name]
        self.log_handler.write_to_engine_log()

    def _run(self):
        try:
            # Proactor loop could miss connection cleanup when client resets connection, then server never finishes stopping.
            asyncio.run(self._serve(), loop_factory=asyncio.SelectorEventLoop)
        except BaseException:
            logger.exception("MCP server stopped because of an error")
        finally:
            self.loop = None

    async def _serve(self):
        # Imported here, because import takes a second or more and should not block main thread.
        import uvicorn
        from sse_starlette.sse import AppStatus

        application = self._build_application()
        self.uvicorn_server = uvicorn.Server(uvicorn.Config(
            application,
            log_config=None,
            access_log=False,
            log_level="warning",
            lifespan="on",
            ws="none",
            timeout_graceful_shutdown=GRACEFUL_SHUTDOWN_TIMEOUT_IN_SECONDS,
        ))
        # stop() sets stopping before it reads uvicorn_server, so server is either stopped here or by should_exit.
        if self.stopping.is_set():
            return

        self.loop = asyncio.get_running_loop()
        AppStatus.should_exit = False
        report_task = asyncio.create_task(self._report_start())
        try:
            await self.uvicorn_server.serve(sockets=[self.socket])
        finally:
            report_task.cancel()

    async def _report_start(self):
        while not self.uvicorn_server.started:
            await asyncio.sleep(0.05)

        logger.info("MCP server listening on %s", self.url)

    def _build_application(self):
        import jsonschema
        import mcp_types
        from mcp.server.lowlevel import NotificationOptions, Server
        from mcp.server.subscriptions import InMemorySubscriptionBus, ListenHandler
        from mcp.server.transport_security import TransportSecuritySettings

        self.types = mcp_types
        self.jsonschema = jsonschema
        self.subscription_bus = InMemorySubscriptionBus()
        self.listen_handler = ListenHandler(self.subscription_bus)

        lowlevel_server = Server(
            SERVER_NAME,
            version=SERVER_VERSION,
            instructions=INSTRUCTIONS,
            get_tool_input_schema=self._get_tool_input_schema,
            on_list_tools=self._list_tools,
            on_call_tool=self._call_tool,
            on_subscriptions_listen=self.listen_handler,
        )
        # Streamable HTTP runner calls it without arguments, without override handshake-era clients would never get tools.listChanged capability.
        create_options = lowlevel_server.create_initialization_options
        lowlevel_server.create_initialization_options = lambda notification_options=None, experimental_capabilities=None, extensions=None: create_options(
            NotificationOptions(tools_changed=True), experimental_capabilities, extensions)
        lowlevel_server.middleware.append(self._track_session)

        security_settings = TransportSecuritySettings(
            enable_dns_rebinding_protection=True,
            allowed_hosts=["127.0.0.1:*", "localhost:*", "[::1]:*"],
            allowed_origins=["http://127.0.0.1:*", "http://localhost:*", "http://[::1]:*"],
        )
        return lowlevel_server.streamable_http_app(streamable_http_path="/mcp", transport_security=security_settings, host=HOST)

    async def _track_session(self, context, call_next):
        request = context.request
        session_id = request.headers.get("mcp-session-id") if request is not None else None
        if session_id:
            self.sessions[session_id] = context.session

        return await call_next(context)

    def _get_tool_input_schema(self, name):
        tool = self.tools.get(name)
        return tool.input_schema if tool is not None else None

    async def _list_tools(self, context, params):
        types = self.types
        return types.ListToolsResult(tools=[
            types.Tool(name=tool.name, description=tool.description, input_schema=tool.input_schema,
                       annotations=types.ToolAnnotations(**tool.annotations) if tool.annotations else None)
            for tool in self.tools.values()
        ])

    async def _call_tool(self, context, params):
        tool = self.tools.get(params.name)
        if tool is None:
            return self._error_result(f"Unknown tool {params.name}")

        arguments = params.arguments or {}
        if tool.validator is None:
            tool.validator = self.jsonschema.Draft202012Validator(tool.input_schema)
        errors = list(tool.validator.iter_errors(arguments))
        if errors:
            return self._error_result("Invalid arguments: " + "; ".join(error.message for error in errors[:5]))

        def run_tool():
            self.log_handler.write_to_engine_log()
            return tool.handler(arguments)

        try:
            result = await self._run_on_main_thread(run_tool)
        except tools.ToolError as error:
            return self._error_result(str(error))
        except Exception as error:
            return self._error_result(f"{type(error).__name__}: {error}")

        if isinstance(result, tools.ToolOutput):
            content = []
            if result.text or not result.image_base64:
                content.append(self.types.TextContent(text=result.text))
            if result.image_base64:
                content.append(self.types.ImageContent(data=result.image_base64, mime_type=result.image_mime_type))
            return self.types.CallToolResult(content=content, is_error=result.is_error)

        return self.types.CallToolResult(content=[self.types.TextContent(text=json.dumps(result, ensure_ascii=False))])

    def _error_result(self, text):
        return self.types.CallToolResult(content=[self.types.TextContent(text=text)], is_error=True)

    async def _run_on_main_thread(self, function):
        future = focalengine.call_on_main_thread(function)
        with self.pending_futures_lock:
            self.pending_futures.add(future)

        try:
            awaitable = asyncio.wrap_future(future)
            try:
                done, _ = await asyncio.wait({awaitable}, timeout=QUEUED_CALL_TIMEOUT_IN_SECONDS)
            except asyncio.CancelledError:
                # Client is gone, call that did not start yet should not change anything.
                future.cancel()
                raise

            # Call that already started could not be cancelled, its result is awaited, so it is never reported as failed while it changed something.
            if not done and future.cancel():
                raise tools.ToolError(f"Engine did not run the call in {QUEUED_CALL_TIMEOUT_IN_SECONDS:g} seconds, for example because a modal dialog is open "
                                      "or the application does not run frames, nothing was changed")

            try:
                return await awaitable
            except asyncio.CancelledError:
                if asyncio.current_task().cancelling():
                    raise

                raise tools.ToolError("Engine is stopping, the call was not run") from None
        finally:
            with self.pending_futures_lock:
                self.pending_futures.discard(future)

    def notify_tools_changed(self):
        loop = self.loop
        if loop is not None and not loop.is_closed():
            asyncio.run_coroutine_threadsafe(self._broadcast_tools_changed(), loop)

    async def _broadcast_tools_changed(self):
        from mcp.server.subscriptions import ToolsListChanged
        from mcp_types.version import HANDSHAKE_PROTOCOL_VERSIONS

        await self.subscription_bus.publish(ToolsListChanged())
        for session_id, session in list(self.sessions.items()):
            if session.protocol_version not in HANDSHAKE_PROTOCOL_VERSIONS:
                continue

            try:
                await session.send_tool_list_changed()
            except Exception:
                self.sessions.pop(session_id, None)

    def is_running(self):
        return self.thread is not None and self.thread.is_alive()

    def stop(self, timeout=3.0):
        if self.thread is None:
            return True

        self.stopping.set()
        with self.pending_futures_lock:
            pending_futures = list(self.pending_futures)
        for future in pending_futures:
            future.cancel()

        loop = self.loop
        if loop is not None and self.listen_handler is not None:
            try:
                # Ends subscriptions/listen streams of 2026-07-28 clients.
                loop.call_soon_threadsafe(self.listen_handler.close)
            except RuntimeError:
                pass

        # Flag is process-wide, sse-starlette ends standalone SSE streams only when it is set, because uvicorn signal handlers do not work on this thread.
        sse = sys.modules.get("sse_starlette.sse")
        if sse is not None:
            sse.AppStatus.should_exit = True

        uvicorn_server = self.uvicorn_server
        if uvicorn_server is not None:
            uvicorn_server.should_exit = True

        self.thread.join(timeout)
        stopped = not self.thread.is_alive()
        # Otherwise SSE responses of next server would end right away.
        if sse is not None:
            sse.AppStatus.should_exit = False

        self.socket.close()
        self.sessions.clear()
        if self.shutdown_callback_id is not None:
            focalengine.remove_callback(self.shutdown_callback_id)
            self.shutdown_callback_id = None

        self._detach_log_handler()
        if stopped:
            focalengine.log("MCP server stopped")
        else:
            focalengine.log(f"MCP server thread did not stop in {timeout:g} seconds", "warning")

        self.thread = None
        return stopped


def start(port=5187):
    """Starts server on 127.0.0.1, returns right away, server reports to engine log when it listens."""
    global _server
    if _server is not None and _server.is_running():
        raise RuntimeError(f"MCP server is already running on {_server.url}")

    if sys.version_info[:2] != (3, 13):
        raise RuntimeError(f"MCP server packages are built for Python 3.13, but Python is {sys.version.split()[0]}")

    if os.environ.get(IMPORT_MARKER) == "1" and "mcp" not in sys.modules:
        raise RuntimeError("MCP server packages were loaded by Python that was restarted after that, they could not be loaded again, restart application")

    packages_directory = _get_packages_directory()
    # Processes .pth files too, pywin32 that mcp imports on Windows needs them.
    if packages_directory not in sys.path:
        site.addsitedir(packages_directory)

    server = _EngineServer(port)
    server.load_native_tools()
    server.start()
    _server = server


def stop(timeout=3.0):
    """Stops server and waits for its thread at most timeout seconds, safe to call when server is not running."""
    global _server
    if _server is None:
        return True

    stopped = _server.stop(timeout)
    if stopped:
        _server = None

    return stopped


def reload_native_tools():
    """MCPPlugin calls it on main thread when tools registered from C++ changed, clients are told that tool list changed."""
    if _server is None or not _server.is_running():
        return

    _server.load_native_tools()
    _server.notify_tools_changed()


def is_running():
    return _server is not None and _server.is_running()


def get_url():
    return _server.url if _server is not None else None