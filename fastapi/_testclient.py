"""Native TestClient — replaces starlette.testclient.

Uses httpx directly with its built-in ASGITransport.
"""
from __future__ import annotations

from typing import Any


class TestClient:
    """ASGI test client backed by httpx.

    Parameters
    ----------
    app : ASGIApp
        The ASGI application to test.
    base_url : str
        Base URL for requests (default: "http://testserver").
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
        transport = httpx.ASGITransport(app=app, raise_app_exceptions=raise_server_exceptions)
        self._client = httpx.Client(
            transport=transport,
            base_url=base_url,
            **kwargs,
        )
        self.app = app

    def __enter__(self) -> "TestClient":
        return self

    def __exit__(self, *args: Any) -> None:
        self._client.close()

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

    def websocket_connect(self, url: str, **kwargs: Any) -> Any:
        """Create a WebSocket connection (requires httpx-ws)."""
        try:
            from httpx_ws import WebSocketSession
            from httpx_ws.transport import ASGIWebSocketTransport
        except ImportError:
            raise RuntimeError(
                "httpx-ws must be installed for WebSocket testing. "
                "Install it with: pip install httpx-ws"
            )
        raise NotImplementedError(
            "WebSocket testing requires httpx-ws. "
            "Use httpx_ws.WebSocketSession directly."
        )

    def close(self) -> None:
        self._client.close()
