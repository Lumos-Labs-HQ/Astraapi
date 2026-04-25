"""Native TestClient — starts C++ server on ephemeral port, uses httpx HTTP.

Tests the real production path (C++ server) instead of ASGI transport.

The server starts lazily on the first HTTP request, so importing this module
(during pytest collection) does NOT open sockets or file descriptors.
"""
from __future__ import annotations

import asyncio
import json
import socket
import threading
from typing import Any


class WebSocketTestSession:
    """Synchronous WebSocket test session backed by the C++ WebSocket server.

    All recv() calls are routed through the _run thread to avoid concurrency issues.
    """

    def __init__(self, url: str, _explicit_close_flag: list | None = None,
                 _active_count: list | None = None) -> None:
        self._url = url
        self._explicit_close_flag = _explicit_close_flag  # shared [bool] with TestClient
        self._active_count = _active_count  # shared [int] active session count
        self._ws: Any = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._closed = threading.Event()
        self._error: BaseException | None = None
        self._server_closed: bool = False
        self._client_closed: bool = False
        # Queue for messages received by _run thread
        import queue
        self._recv_queue: queue.Queue = queue.Queue()

    def __enter__(self) -> "WebSocketTestSession":
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout=10.0):
            raise RuntimeError("WebSocket connection timed out")
        if self._error:
            try:
                import websockets.exceptions as _wse
                from astraapi._websocket import WebSocketDisconnect
                if isinstance(self._error, (_wse.ConnectionClosedOK, _wse.ConnectionClosedError)):
                    rcvd = getattr(self._error, "rcvd", None)
                    if rcvd is not None:
                        code = getattr(rcvd, "code", 1000)
                        reason = getattr(rcvd, "reason", "") or ""
                        if reason.startswith("1006:"):
                            code = 1006
                            reason = reason[5:]
                    else:
                        proto = getattr(self._error, "protocol", None)
                        code = getattr(proto, "close_code", 1006) if proto else 1006
                        reason = ""
                    raise WebSocketDisconnect(code=code, reason=reason)
            except ImportError:
                pass
            raise self._error
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        try:
            self._close_sync()
        except Exception:
            pass
        _flag = self._explicit_close_flag
        if exc_type is None and not self._client_closed and not self._server_closed and not (_flag and _flag[0]):
            from astraapi._websocket import WebSocketDisconnect
            raise WebSocketDisconnect(code=1000)

    def _run(self) -> None:
        try:
            import websockets.sync.client as _ws_client  # type: ignore[import]
        except ImportError:
            self._error = ImportError("websockets must be installed: pip install websockets")
            self._ready.set()
            return
        try:
            self._ws = _ws_client.connect(self._url, compression=None)
            # Peek: detect immediate server close before signaling ready
            try:
                msg = self._ws.recv(timeout=5.0)
                self._recv_queue.put(msg)
            except TimeoutError:
                pass  # no immediate message, server still open
            except Exception as exc:
                self._error = exc
                self._ready.set()
                return
            self._ready.set()
            # Continuously recv until closed
            while not self._closed.is_set():
                try:
                    msg = self._ws.recv(timeout=2.5)
                    self._recv_queue.put(msg)
                except TimeoutError:
                    pass
                except Exception:
                    self._server_closed = True
                    self._recv_queue.put(None)  # sentinel: server closed
                    break
        except Exception as exc:
            self._error = exc
            self._ready.set()
        finally:
            if self._ws:
                try:
                    self._ws.close()
                except Exception:
                    pass

    def _close_sync(self) -> None:
        self._closed.set()
        if self._thread:
            self._thread.join(timeout=5.0)
        if self._active_count is not None and self._active_count[0] > 0:
            self._active_count[0] -= 1

    def _recv_msg(self) -> Any:
        """Get next message from queue, raising WebSocketDisconnect if server closed."""
        import queue
        while True:
            try:
                msg = self._recv_queue.get(timeout=30.0)
                if msg is None:
                    from astraapi._websocket import WebSocketDisconnect
                    raise WebSocketDisconnect(code=1000)
                return msg
            except queue.Empty:
                raise RuntimeError("WebSocket receive timed out")

    def send_text(self, data: str) -> None:
        if self._ws is None:
            raise RuntimeError("WebSocket not connected")
        self._ws.send(data)

    def send_bytes(self, data: bytes) -> None:
        if self._ws is None:
            raise RuntimeError("WebSocket not connected")
        self._ws.send(data)

    def send_json(self, data: Any, mode: str = "text") -> None:
        text = json.dumps(data)
        if mode == "binary":
            self.send_bytes(text.encode("utf-8"))
        else:
            self.send_text(text)

    def receive_text(self) -> str:
        msg = self._recv_msg()
        if isinstance(msg, bytes):
            return msg.decode("utf-8")
        return msg

    def receive_bytes(self) -> bytes:
        msg = self._recv_msg()
        if isinstance(msg, str):
            return msg.encode("utf-8")
        return msg

    def receive_json(self, mode: str = "text") -> Any:
        if mode == "binary":
            return json.loads(self.receive_bytes())
        return json.loads(self.receive_text())

    def close(self, code: int = 1000) -> None:
        self._client_closed = True
        if self._explicit_close_flag is not None:
            self._explicit_close_flag[0] = True
        if self._ws:
            try:
                self._ws.close()
            except Exception:
                pass
        self._close_sync()


# ── Per-app shared server registry ───────────────────────────────────────────
# Maps id(app) -> _SharedServer. All TestClient instances for the same app
# share one server thread. A cap of _MAX_LIVE_SERVERS prevents thread
# exhaustion when hundreds of test modules each create a unique app.

_MAX_LIVE_SERVERS = 64  # generous cap — crash was C++ bug, not thread count


class _SharedServer:
    __slots__ = ('port', 'thread', 'loop_ref', 'stop_event')

    def __init__(self):
        self.port: int | None = None
        self.thread: threading.Thread | None = None
        self.loop_ref: asyncio.AbstractEventLoop | None = None
        self.stop_event: asyncio.Event | None = None

    def stop(self) -> None:
        if self.stop_event is not None and self.loop_ref is not None:
            try:
                if not self.loop_ref.is_closed():
                    self.loop_ref.call_soon_threadsafe(self.stop_event.set)
            except Exception:
                pass
        self.stop_event = None
        if self.thread is not None:
            self.thread.join(timeout=3.0)
        self.thread = None


from collections import OrderedDict as _OrderedDict
_app_servers: "_OrderedDict[int, _SharedServer]" = _OrderedDict()
_app_servers_lock = threading.Lock()


class TestClient:
    """Test client backed by the C++ HTTP server.

    Starts the server lazily on the first HTTP request (not at construction
    time), so creating many TestClient instances during pytest collection
    does not exhaust file descriptors.

    Parameters
    ----------
    app : AstraAPI
        The AstraAPI application to test.
    base_url : str
        Ignored (kept for API compatibility). The actual URL is
        ``http://127.0.0.1:<ephemeral_port>``.
    """
    __test__ = False  # prevent pytest from collecting this as a test class

    def __init__(
        self,
        app: Any,
        base_url: str = "http://testserver",
        raise_server_exceptions: bool = True,
        root_path: str = "",
        **kwargs: Any,
    ) -> None:
        import httpx
        self.app = app
        self._root_path = root_path
        self._httpx_kwargs = kwargs
        # Server state — all None until _ensure_started() is called
        self._port: int | None = None
        self._base_url: str | None = None
        self._stop: threading.Event | None = None
        self._started: threading.Event | None = None
        self._thread: threading.Thread | None = None
        self._client: Any = None
        self._server_error: BaseException | None = None
        self._lock = threading.Lock()
        self._loop_ref: asyncio.AbstractEventLoop | None = None
        self._stop_event: asyncio.Event | None = None
        self._shared_server: "_SharedServer | None" = None
        # Mutable headers dict — tests can call client.headers.clear() etc.
        # Populated with starlette-compatible default so tests expecting
        # 'User-Agent: testclient' pass even without explicit headers.
        self._init_base_url = base_url
        _default_headers = {"user-agent": "testclient"}
        # Extract host from base_url for Host header
        try:
            import urllib.parse as _up
            _parsed = _up.urlparse(base_url)
            if _parsed.hostname:
                _host = _parsed.hostname
                if _parsed.port and _parsed.port not in (80, 443):
                    _host = f"{_host}:{_parsed.port}"
                _default_headers["host"] = _host
            if _parsed.scheme == "https":
                _default_headers["x-forwarded-proto"] = "https"
        except Exception:
            pass
        self.headers: Any = httpx.Headers(_default_headers)
        self.cookies: Any = httpx.Cookies()
        self._raise_server_exceptions = raise_server_exceptions

    @property
    def base_url(self) -> str:
        """Return the base URL of the test server."""
        self._ensure_started()
        return self._base_url or "http://testserver"

    def _ensure_started(self) -> None:
        """Start the server on first use — shared per app instance."""
        with self._lock:
            if self._client is not None:
                # Recreate httpx client if closed
                try:
                    import httpx
                    if getattr(self._client, '_state', None) is not None and self._client._state.name == 'CLOSED':
                        kw = dict(self._httpx_kwargs)
                        kw.setdefault('follow_redirects', True)
                        _headers = dict(self.headers)
                        _headers.setdefault('host', 'testserver')
                        self._client = httpx.Client(base_url=self._base_url, headers=_headers, **kw)
                except Exception:
                    pass
                return

            try:
                import httpx
            except ImportError:
                raise RuntimeError("httpx must be installed to use TestClient. Install it with: pip install httpx")

            app_id = id(self.app)
            with _app_servers_lock:
                shared = _app_servers.get(app_id)
                need_start = shared is None or shared.thread is None or not shared.thread.is_alive()

                if need_start:
                    shared = _SharedServer()
                    _app_servers[app_id] = shared

                    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                        s.bind(("127.0.0.1", 0))
                        shared.port = s.getsockname()[1]

                    started_event = threading.Event()
                    self._server_error = None

                    def _run(_shared=shared):
                        try:
                            import uvloop
                            loop = uvloop.new_event_loop()
                        except ImportError:
                            try:
                                import winloop
                                loop = winloop.new_event_loop()
                            except ImportError:
                                loop = asyncio.new_event_loop()
                        asyncio.set_event_loop(loop)
                        _shared.loop_ref = loop
                        import gc; gc.disable()
                        try:
                            loop.run_until_complete(self._serve_shared(loop, _shared, started_event))
                        except Exception as exc:
                            self._server_error = exc
                            started_event.set()
                        finally:
                            gc.enable()
                            loop.close()

                    shared.thread = threading.Thread(target=_run, daemon=True)
                    shared.thread.start()
                    _app_servers_lock.release()
                    try:
                        if not started_event.wait(timeout=10.0):
                            raise RuntimeError("C++ test server failed to start within 10s")
                        if self._server_error is not None:
                            err = self._server_error
                            # Re-raise original exception type if possible
                            if isinstance(err, (ValueError, TypeError)):
                                raise err
                            raise RuntimeError(f"Server startup failed: {err}")
                    finally:
                        _app_servers_lock.acquire()
                else:
                    _app_servers.move_to_end(app_id)

                self._port = shared.port
                self._base_url = f"http://127.0.0.1:{shared.port}"
                self._shared_server = shared

            kw = dict(self._httpx_kwargs)
            kw.setdefault('follow_redirects', True)
            _headers = dict(self.headers)
            _headers.setdefault('host', 'testserver')
            self._client = httpx.Client(base_url=self._base_url, headers=_headers, **kw)

    async def _serve_shared(self, loop: asyncio.AbstractEventLoop, shared: "_SharedServer", started_event: threading.Event) -> None:
        from astraapi._cpp_server import _create_server, _set_raise_server_exceptions
        _set_raise_server_exceptions(self._raise_server_exceptions)
        _lifespan_cm = None
        # Clear any stale lifespan state from previous test
        try:
            from astraapi._cpp_server import _set_lifespan_state
            _set_lifespan_state({})
        except Exception:
            pass
        _router = getattr(self.app, "router", None)
        _lh = getattr(_router, "lifespan_context", None) if _router else None
        if _lh is not None:
            _lifespan_cm = _lh(self.app)
            _ls_state = await _lifespan_cm.__aenter__()
            if _ls_state is not None:
                _app_state = getattr(self.app, "state", None)
                if _app_state is not None:
                    _sd = getattr(_app_state, "_state", None)
                    if isinstance(_sd, dict):
                        _sd.update(_ls_state)
                    elif isinstance(_ls_state, dict):
                        for _k, _v in _ls_state.items():
                            setattr(_app_state, _k, _v)
                # Also store in module-level for request scope injection
                try:
                    from astraapi._cpp_server import _set_lifespan_state
                    _set_lifespan_state(_ls_state)
                except Exception:
                    pass
        server = await _create_server(self.app, "127.0.0.1", shared.port, root_path=self._root_path)
        stop_event = asyncio.Event()
        shared.stop_event = stop_event
        started_event.set()
        await stop_event.wait()
        server.close()
        await server.wait_closed()
        if _lifespan_cm is not None:
            await _lifespan_cm.__aexit__(None, None, None)
        # Clear lifespan state after server stops
        try:
            from astraapi._cpp_server import _set_lifespan_state
            _set_lifespan_state({})
        except Exception:
            pass

    # -- HTTP methods (all lazy-start) ---------------------------------------

    def _check_exc(self) -> None:
        """Re-raise any server exception captured during the last request."""
        from astraapi._cpp_server import _pop_server_exception
        from astraapi.exceptions import ResponseValidationError
        exc = _pop_server_exception()  # always pop to prevent cross-test pollution
        if exc is not None:
            # Always re-raise ResponseValidationError (programming error, not server error)
            if isinstance(exc, ResponseValidationError) or self._raise_server_exceptions:
                raise exc

    def _sync_raise_flag(self) -> None:
        """Sync _raise_server_exceptions global to this client's setting before each request."""
        from astraapi._cpp_server import _set_raise_server_exceptions
        _set_raise_server_exceptions(self._raise_server_exceptions)

    def _apply_cookies(self) -> None:
        """Sync instance cookies to the underlying httpx client."""
        if self._client is not None and self.cookies:
            _cookies = self.cookies
            if isinstance(_cookies, list):
                _cookies = dict(_cookies)
            for name, value in _cookies.items():
                self._client.cookies.set(name, value)


    def get(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.get(url, **kwargs)
        self._check_exc()
        return resp

    def post(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.post(url, **kwargs)
        self._check_exc()
        return resp

    def put(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.put(url, **kwargs)
        self._check_exc()
        return resp

    def patch(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.patch(url, **kwargs)
        self._check_exc()
        return resp

    def delete(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.delete(url, **kwargs)
        self._check_exc()
        return resp

    def options(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        resp = self._client.options(url, **kwargs)
        self._check_exc()
        return resp

    def head(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        resp = self._client.head(url, **kwargs)
        self._check_exc()
        return resp

    def request(self, method: str, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        self._sync_raise_flag()
        self._apply_cookies()
        resp = self._client.request(method, url, **kwargs)
        self._check_exc()
        return resp

    def websocket_connect(
        self, url: str, subprotocols: list[str] | None = None, **kwargs: Any
    ) -> "WebSocketTestSession":
        """Open a WebSocket connection to the running C++ test server.

        Returns a context-manager ``WebSocketTestSession`` that provides
        ``send_text``, ``receive_text``, ``send_json``, ``receive_json``,
        ``send_bytes``, ``receive_bytes``, and ``close``.
        """
        self._ensure_started()
        # Convert http:// base_url to ws:// WebSocket URL
        ws_url = f"ws://127.0.0.1:{self._port}{url}"
        if not hasattr(self, "_ws_close_flag"):
            self._ws_close_flag: list = [False]
            self._ws_active: list = [0]
        self._ws_active[0] += 1
        if self._ws_active[0] == 1:
            self._ws_close_flag[0] = False  # reset for new group
        return WebSocketTestSession(ws_url, _explicit_close_flag=self._ws_close_flag,
                                    _active_count=self._ws_active)

    # -- Lifecycle -----------------------------------------------------------

    def close(self) -> None:
        """Close the httpx client and release the shared server reference."""
        if self._client is not None:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None
        self._stop_server()

    def __del__(self) -> None:
        try:
            self._stop_server()
        except Exception:
            pass

    def _stop_server(self) -> None:
        """Remove this client's shared server reference (server stays alive for other clients)."""
        self._shared_server = None
        self._thread = None

    def __enter__(self) -> "TestClient":
        self._ensure_started()
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()
        # When used as context manager, actually stop the server so lifespan shutdown runs
        shared = self._shared_server
        if shared is None:
            # Already cleared by close() — look up by app id
            app_id = id(self.app)
            with _app_servers_lock:
                shared = _app_servers.pop(app_id, None)
        else:
            self._shared_server = None
            app_id = id(self.app)
            with _app_servers_lock:
                _app_servers.pop(app_id, None)
        if shared is not None:
            shared.stop()

    @property
    def app_state(self) -> Any:
        """Return the lifespan state dict from app.state, or None if empty."""
        app_state = getattr(self.app, "state", None)
        if app_state is None:
            return None
        state_dict = getattr(app_state, "_state", {})
        return state_dict if state_dict else None

