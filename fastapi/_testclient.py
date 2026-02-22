"""Native TestClient — starts C++ server on ephemeral port, uses httpx HTTP.

Tests the real production path (C++ server) instead of ASGI transport.
"""
from __future__ import annotations

import asyncio
import socket
import threading
from typing import Any


class TestClient:
    """Test client backed by the C++ HTTP server.

    Starts the server on an ephemeral port in a background thread and
    sends real HTTP requests via httpx.

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
        try:
            import httpx
        except ImportError:
            raise RuntimeError(
                "httpx must be installed to use TestClient. "
                "Install it with: pip install httpx"
            )

        self.app = app

        # Find a free port
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            self._port = s.getsockname()[1]

        self._base_url = f"http://127.0.0.1:{self._port}"
        self._started = threading.Event()
        self._stop = threading.Event()
        self._server_error: BaseException | None = None
        self._thread = threading.Thread(target=self._run_server, daemon=True)
        self._thread.start()

        if not self._started.wait(timeout=10.0):
            raise RuntimeError("C++ test server failed to start within 10s")
        if self._server_error is not None:
            raise RuntimeError(f"Server startup failed: {self._server_error}")

        self._client = httpx.Client(base_url=self._base_url, **kwargs)

    def _run_server(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._serve(loop))
        except Exception as exc:
            self._server_error = exc
            self._started.set()
        finally:
            loop.close()

    async def _serve(self, loop: asyncio.AbstractEventLoop) -> None:
        from fastapi._cpp_server import _create_server

        server = await _create_server(
            self.app, "127.0.0.1", self._port,
        )
        self._started.set()

        # Wait until close() is called
        while not self._stop.is_set():
            await asyncio.sleep(0.05)

        server.close()
        await server.wait_closed()

    # -- HTTP methods --------------------------------------------------------

    def get(self, url: str, **kwargs: Any) -> Any:
        return self._client.get(url, **kwargs)

    def post(self, url: str, **kwargs: Any) -> Any:
        return self._client.post(url, **kwargs)

    def put(self, url: str, **kwargs: Any) -> Any:
        return self._client.put(url, **kwargs)

    def patch(self, url: str, **kwargs: Any) -> Any:
        return self._client.patch(url, **kwargs)

    def delete(self, url: str, **kwargs: Any) -> Any:
        return self._client.delete(url, **kwargs)

    def options(self, url: str, **kwargs: Any) -> Any:
        return self._client.options(url, **kwargs)

    def head(self, url: str, **kwargs: Any) -> Any:
        return self._client.head(url, **kwargs)

    def request(self, method: str, url: str, **kwargs: Any) -> Any:
        return self._client.request(method, url, **kwargs)

    # -- Lifecycle -----------------------------------------------------------

    def close(self) -> None:
        self._client.close()
        self._stop.set()
        self._thread.join(timeout=5.0)

    def __enter__(self) -> "TestClient":
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()
