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

    Provides the same API as starlette's WebSocketTestSession so existing
    tests work without modification.
    """

    def __init__(self, url: str) -> None:
        self._url = url
        self._ws: Any = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._ready = threading.Event()
        self._closed = threading.Event()
        self._send_q: list[Any] = []
        self._recv_q: list[Any] = []
        self._lock = threading.Lock()
        self._error: BaseException | None = None

    def __enter__(self) -> "WebSocketTestSession":
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout=10.0):
            raise RuntimeError("WebSocket connection timed out")
        if self._error:
            raise self._error
        return self

    def __exit__(self, *args: Any) -> None:
        try:
            self._close_sync()
        except Exception:
            pass

    def _run(self) -> None:
        try:
            import websockets.sync.client as _ws_client  # type: ignore[import]
        except ImportError:
            try:
                import websockets.sync.client as _ws_client  # type: ignore[import]
            except ImportError:
                self._error = ImportError("websockets must be installed: pip install websockets")
                self._ready.set()
                return
        try:
            self._ws = _ws_client.connect(self._url)
            self._ready.set()
            # Keep thread alive until close() is called
            self._closed.wait()
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
        if self._ws is None:
            raise RuntimeError("WebSocket not connected")
        msg = self._ws.recv()
        if isinstance(msg, bytes):
            return msg.decode("utf-8")
        return msg

    def receive_bytes(self) -> bytes:
        if self._ws is None:
            raise RuntimeError("WebSocket not connected")
        msg = self._ws.recv()
        if isinstance(msg, str):
            return msg.encode("utf-8")
        return msg

    def receive_json(self, mode: str = "text") -> Any:
        if mode == "binary":
            return json.loads(self.receive_bytes())
        return json.loads(self.receive_text())

    def close(self, code: int = 1000) -> None:
        if self._ws:
            try:
                self._ws.close()
            except Exception:
                pass
        self._close_sync()


class TestClient:
    """Test client backed by the C++ HTTP server.

    Starts the server lazily on the first HTTP request (not at construction
    time), so creating many TestClient instances during pytest collection
    does not exhaust file descriptors.

    Parameters
    ----------
    app : FastAPI
        The FastAPI application to test.
    base_url : str
        Ignored (kept for API compatibility). The actual URL is
        ``http://127.0.0.1:<ephemeral_port>``.
    """

    def __init__(
        self,
        app: Any,
        base_url: str = "http://testserver",
        raise_server_exceptions: bool = True,
        **kwargs: Any,
    ) -> None:
        import httpx
        self.app = app
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
        # Mutable headers dict — tests can call client.headers.clear() etc.
        # Populated with starlette-compatible default so tests expecting
        # 'User-Agent: testclient' pass even without explicit headers.
        self.headers: Any = httpx.Headers({"user-agent": "testclient"})

    def _ensure_started(self) -> None:
        """Start the server on first use (lazy init — no FDs opened at construction)."""
        with self._lock:
            # Recreate httpx client if it was closed (e.g. after exiting a `with` block)
            if self._client is not None and getattr(self._client, '_state', None) is not None:
                try:
                    import httpx
                    if self._client._state.name == 'CLOSED':
                        kw = dict(self._httpx_kwargs)
                        kw.setdefault('follow_redirects', True)
                        self._client = httpx.Client(
                            base_url=self._base_url,
                            headers=dict(self.headers),
                            **kw,
                        )
                        return
                except Exception:
                    pass
            if self._client is not None:
                return
            try:
                import httpx
            except ImportError:
                raise RuntimeError(
                    "httpx must be installed to use TestClient. "
                    "Install it with: pip install httpx"
                )
            # Find a free port
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.bind(("127.0.0.1", 0))
                self._port = s.getsockname()[1]
            self._base_url = f"http://127.0.0.1:{self._port}"
            self._started = threading.Event()
            self._stop = threading.Event()
            self._server_error = None
            self._thread = threading.Thread(target=self._run_server, daemon=True)
            self._thread.start()
            if not self._started.wait(timeout=10.0):
                raise RuntimeError("C++ test server failed to start within 10s")
            if self._server_error is not None:
                raise RuntimeError(f"Server startup failed: {self._server_error}")
            kw = dict(self._httpx_kwargs)
            kw.setdefault('follow_redirects', True)
            self._client = httpx.Client(
                base_url=self._base_url,
                headers=dict(self.headers),
                **kw,
            )

    def _run_server(self) -> None:
        # Use uvloop (Linux) or winloop (Windows) — same loop as production.
        # Fall back to plain asyncio when neither is installed.
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
        self._loop_ref = loop  # store for close() to use call_soon_threadsafe
        # Disable GC to match production run_server() — prevents C-extension object
        # collection during active request processing (avoids use-after-free segfaults).
        import gc
        gc.disable()
        gc.collect()
        try:
            loop.run_until_complete(self._serve(loop))
        except Exception as exc:
            self._server_error = exc
            self._started.set()  # type: ignore[union-attr]
        finally:
            gc.enable()
            loop.close()

    async def _serve(self, loop: asyncio.AbstractEventLoop) -> None:
        from fastapi._cpp_server import _create_server
        server = await _create_server(self.app, "127.0.0.1", self._port)
        # Use asyncio.Event for clean shutdown instead of polling every 50ms.
        # The stop event is set by close() via loop.call_soon_threadsafe().
        stop_event = asyncio.Event()
        self._stop_event = stop_event
        self._started.set()  # type: ignore[union-attr]
        await stop_event.wait()
        server.close()
        await server.wait_closed()

    # -- HTTP methods (all lazy-start) ---------------------------------------

    def get(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.get(url, **kwargs)

    def post(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.post(url, **kwargs)

    def put(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.put(url, **kwargs)

    def patch(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.patch(url, **kwargs)

    def delete(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.delete(url, **kwargs)

    def options(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.options(url, **kwargs)

    def head(self, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.head(url, **kwargs)

    def request(self, method: str, url: str, **kwargs: Any) -> Any:
        self._ensure_started()
        return self._client.request(method, url, **kwargs)

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
        return WebSocketTestSession(ws_url)

    # -- Lifecycle -----------------------------------------------------------

    def close(self) -> None:
        """Close the httpx client. The server thread continues running (daemon)
        so that the same TestClient instance can be reused across multiple tests.
        The server is stopped when the process exits (daemon thread).
        """
        if self._client is not None:
            try:
                self._client.close()
            except Exception:
                pass

    def __enter__(self) -> "TestClient":
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()

