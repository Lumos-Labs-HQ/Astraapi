"""
Native WebSocket class — replaces starlette.websockets.

Provides full ASGI WebSocket support with zero starlette imports.
Uses C++ ``ws_parse_json`` / ``ws_serialize_json`` when available.
"""
from __future__ import annotations

import enum
from fastapi._json_utils import json_dumps_str as _json_dumps_str, json_loads as _json_loads
from typing import Any, Optional

from fastapi._request import HTTPConnection
from fastapi._types import Receive, Scope, Send

# Optional C++ accelerated JSON for WebSocket payloads
try:
    from fastapi._core_bridge import encode_to_json_bytes as _core_ws_serialize

    _CORE_WS_JSON = True
except Exception:
    _CORE_WS_JSON = False


# ---------------------------------------------------------------------------
# WebSocket state & exceptions
# ---------------------------------------------------------------------------


class WebSocketState(enum.Enum):
    """WebSocket connection lifecycle states."""

    CONNECTING = 0
    CONNECTED = 1
    DISCONNECTED = 2


class WebSocketDisconnect(Exception):
    """Raised when a WebSocket disconnects unexpectedly."""

    def __init__(self, code: int = 1000, reason: Optional[str] = None) -> None:
        self.code = code
        self.reason = reason or ""
        super().__init__(f"WebSocket disconnect: code={code}, reason={self.reason}")


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------


class WebSocket(HTTPConnection):
    """Full ASGI WebSocket connection.

    Extends ``HTTPConnection`` with WebSocket-specific accept/send/receive
    lifecycle methods and state tracking.
    """

    def __init__(self, scope: Scope, receive: Optional[Receive] = None, send: Optional[Send] = None) -> None:
        assert scope["type"] == "websocket"
        super().__init__(scope, receive, send)
        self.client_state = WebSocketState.CONNECTING
        self.application_state = WebSocketState.CONNECTING

    @property
    def receive(self) -> Receive:
        assert self._receive is not None, "No receive channel available"
        return self._receive

    @property
    def send(self) -> Send:
        assert self._send is not None, "No send channel available"
        return self._send

    # -- Connection lifecycle ------------------------------------------------

    async def accept(
        self,
        subprotocol: Optional[str] = None,
        headers: Optional[list[tuple[bytes, bytes]]] = None,
    ) -> None:
        """Accept the WebSocket connection."""
        if self.client_state == WebSocketState.CONNECTING:
            # Wait for the client connect message
            message = await self.receive()
            message_type = message["type"]
            if message_type != "websocket.connect":
                raise RuntimeError(
                    f"Expected 'websocket.connect', got '{message_type}'"
                )
            self.client_state = WebSocketState.CONNECTED

        accept_message: dict[str, Any] = {"type": "websocket.accept"}
        if subprotocol is not None:
            accept_message["subprotocol"] = subprotocol
        if headers is not None:
            accept_message["headers"] = headers
        await self.send(accept_message)
        self.application_state = WebSocketState.CONNECTED

    async def close(self, code: int = 1000, reason: Optional[str] = None) -> None:
        """Close the WebSocket connection from the server side."""
        if self.application_state == WebSocketState.CONNECTED:
            close_message: dict[str, Any] = {
                "type": "websocket.close",
                "code": code,
            }
            if reason is not None:
                close_message["reason"] = reason
            await self.send(close_message)
            self.application_state = WebSocketState.DISCONNECTED

    # -- Sending -------------------------------------------------------------

    async def send_text(self, data: str) -> None:
        """Send a text message."""
        self._assert_connected()
        await self.send({"type": "websocket.send", "text": data})

    async def send_bytes(self, data: bytes) -> None:
        """Send a binary message."""
        self._assert_connected()
        await self.send({"type": "websocket.send", "bytes": data})

    async def send_json(self, data: Any, mode: str = "text") -> None:
        """Send a JSON-encoded message.

        Parameters
        ----------
        data : Any
            The data to serialize as JSON.
        mode : str
            ``"text"`` (default) sends as a text frame,
            ``"binary"`` sends as a binary frame.
        """
        self._assert_connected()

        # Try C++ fast-path
        if _CORE_WS_JSON:
            try:
                json_bytes = _core_ws_serialize(data)
                if mode == "text":
                    await self.send(
                        {"type": "websocket.send", "text": json_bytes.decode("utf-8")}
                    )
                else:
                    await self.send({"type": "websocket.send", "bytes": json_bytes})
                return
            except (ValueError, TypeError):
                pass

        text = _json_dumps_str(data)
        if mode == "text":
            await self.send({"type": "websocket.send", "text": text})
        else:
            await self.send(
                {"type": "websocket.send", "bytes": text.encode("utf-8")}
            )

    # -- Receiving -----------------------------------------------------------

    async def receive_text(self) -> str:
        """Receive a text message."""
        self._assert_connected()
        message = await self._receive_message()
        text = message.get("text")
        if text is None:
            raise RuntimeError("Expected text frame, got binary")
        return text

    async def receive_bytes(self) -> bytes:
        """Receive a binary message."""
        self._assert_connected()
        message = await self._receive_message()
        data = message.get("bytes")
        if data is None:
            raise RuntimeError("Expected binary frame, got text")
        return data

    async def receive_json(self, mode: str = "text") -> Any:
        """Receive and parse a JSON message.

        Parameters
        ----------
        mode : str
            ``"text"`` (default) reads from the text frame,
            ``"binary"`` reads from the binary frame.
        """
        self._assert_connected()
        message = await self._receive_message()
        if mode == "text":
            text = message.get("text")
            if text is None:
                raise RuntimeError("Expected text frame, got binary")
            return _json_loads(text)
        else:
            data = message.get("bytes")
            if data is None:
                raise RuntimeError("Expected binary frame, got text")
            return _json_loads(data)

    # -- Iteration -----------------------------------------------------------

    async def __aiter__(self):
        """Iterate over incoming messages until disconnect."""
        try:
            while True:
                message = await self._receive_message()
                text = message.get("text")
                if text is not None:
                    yield {"type": "websocket.receive", "text": text}
                else:
                    yield {"type": "websocket.receive", "bytes": message.get("bytes", b"")}
        except WebSocketDisconnect:
            return

    # -- Internal helpers ----------------------------------------------------

    async def _receive_message(self) -> dict[str, Any]:
        """Receive the next WebSocket message, handling disconnects."""
        if self.client_state == WebSocketState.DISCONNECTED:
            raise WebSocketDisconnect(code=1000)

        message = await self.receive()
        message_type = message["type"]

        if message_type == "websocket.receive":
            return message
        elif message_type == "websocket.disconnect":
            self.client_state = WebSocketState.DISCONNECTED
            code = message.get("code", 1000)
            reason = message.get("reason", "")
            raise WebSocketDisconnect(code=code, reason=reason)
        else:
            raise RuntimeError(f"Unexpected ASGI message type: {message_type}")

    def _assert_connected(self) -> None:
        """Raise if the connection is not in CONNECTED state."""
        if self.application_state != WebSocketState.CONNECTED:
            raise RuntimeError(
                f"WebSocket is not connected (state={self.application_state.name})"
            )
        if self.client_state == WebSocketState.DISCONNECTED:
            raise WebSocketDisconnect(code=1000)
