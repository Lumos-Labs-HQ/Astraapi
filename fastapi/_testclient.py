"""Native TestClient — starts C++ server on ephemeral port, uses httpx HTTP.

Tests the real production path (C++ server) instead of ASGI transport.

The server starts lazily on the first HTTP request, so importing this module
(during pytest collection) does NOT open sockets or file descriptors.
"""
from __future__ import annotations

import asyncio
import socket
import threading
from typing import Any


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

    def _ensure_started(self) -> None:
        """Start the server on first use (lazy init — no FDs opened at construction)."""
        if self._client is not None:
            return
        with self._lock:
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
            self._client = httpx.Client(base_url=self._base_url, **kw)

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
        try:
            loop.run_until_complete(self._serve(loop))
        except Exception as exc:
            self._server_error = exc
            self._started.set()  # type: ignore[union-attr]
        finally:
            loop.close()

    async def _serve(self, loop: asyncio.AbstractEventLoop) -> None:
        from fastapi._cpp_server import _create_server
        server = await _create_server(self.app, "127.0.0.1", self._port)
        self._started.set()  # type: ignore[union-attr]
        while not self._stop.is_set():  # type: ignore[union-attr]
            await asyncio.sleep(0.05)
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

    # -- Lifecycle -----------------------------------------------------------

    def close(self) -> None:
        if self._client is not None:
            self._client.close()
        if self._stop is not None:
            self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)

    def __enter__(self) -> "TestClient":
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()

