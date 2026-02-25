"""
C++ HTTP Server — asyncio.Protocol with uWS/libuv-inspired event loop.

Architecture (modeled on uWebSockets + libuv):
  C++ does ALL fast work: HTTP parse → route match → param extract → JSON serialize
  → CORS → trusted host → DI → compression → response build → transport.write()
  Python does ONLY: buffer management, async endpoint dispatch, WebSocket lifecycle

  Sync endpoints:  processed INLINE by C++ handle_http() — zero Python overhead
  Async endpoints: C++ returns packed tuple, Python awaits + writes via C++
  WebSocket:       C++ sends 101 upgrade, Python handles frame I/O lifecycle
  Pydantic routes: C++ returns InlineResult, Python does Pydantic → C++ serializes
"""

from __future__ import annotations

import asyncio
import inspect
import logging
import os
import signal
import socket
import struct
import sys
import gc
import time
from collections import deque
from typing import Any

from fastapi._websocket import WebSocketDisconnect, WebSocketState

logger = logging.getLogger("fastapi")

# ── Module-level capability checks (avoid per-connection try/except) ──────────
_HAS_QUICKACK = hasattr(socket, "TCP_QUICKACK") or sys.platform == "linux"
_TCP_QUICKACK = 12  # constant for TCP_QUICKACK

# ── Zero-overhead awaitable for sync send methods ────────────────────────────
class _NoopAwaitable:
    """Awaitable that completes immediately with no overhead.
    Used by send_text/send_bytes to avoid coroutine frame creation."""
    __slots__ = ()
    def __await__(self):
        return iter(())

_NOOP = _NoopAwaitable()

# ── HTTP status reason phrases ────────────────────────────────────────────────
_STATUS_PHRASES: dict[int, str] = {
    200: "OK",
    201: "Created",
    204: "No Content",
    301: "Moved Permanently",
    302: "Found",
    304: "Not Modified",
    307: "Temporary Redirect",
    400: "Bad Request",
    401: "Unauthorized",
    403: "Forbidden",
    404: "Not Found",
    405: "Method Not Allowed",
    413: "Payload Too Large",
    422: "Unprocessable Entity",
    500: "Internal Server Error",
    502: "Bad Gateway",
    503: "Service Unavailable",
}
# Pre-computed bytes version — avoids .encode() per streaming response
# Pre-built streaming status line prefixes — avoids 4× bytearray.extend() per streaming response
# Maps status_code → b"HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n"
_STREAMING_STATUS_LINES: dict[int, bytes] = {
    code: f"HTTP/1.1 {code} {phrase}\r\ntransfer-encoding: chunked\r\n".encode()
    for code, phrase in _STATUS_PHRASES.items()
}

# ── Lightweight single-producer single-consumer channel ──────────────────────
class _WsFastChannel:
    """Ultra-fast message channel replacing asyncio.Queue for WebSocket.

    In the common echo pattern, the consumer (endpoint) is already waiting
    when data arrives. feed() directly resolves the Future — no deque,
    no condition variable, no extra allocations.

    Backpressure: pauses transport reading when buffer exceeds high_water,
    resumes when it drops below low_water.
    """
    __slots__ = ('_waiter', '_buffer', '_loop', '_protocol',
                 '_high_water', '_low_water', '_paused',
                 '_total_bytes', '_byte_high_water', '_byte_low_water')

    def __init__(self, loop: asyncio.AbstractEventLoop, protocol=None,
                 high_water: int = 256, low_water: int = 64):
        self._waiter: asyncio.Future | None = None
        self._buffer: deque = deque()  # O(1) popleft vs O(N) list.pop(0)
        self._loop = loop
        self._protocol = protocol  # back-ref for backpressure
        self._high_water = high_water
        self._low_water = low_water
        self._paused = False
        # PERF-2: Byte-count backpressure (prevents 16GB memory with large messages)
        self._total_bytes = 0
        self._byte_high_water = 8_388_608   # 8MB
        self._byte_low_water = 2_097_152    # 2MB

    def feed(self, opcode: int, payload: bytes) -> None:
        """Feed a frame — called from data_received (sync context)."""
        waiter = self._waiter
        if waiter is not None and not waiter.done():
            self._waiter = None
            waiter.set_result((opcode, payload))
        else:
            # Fast type identity check instead of isinstance() — C++ always sends bytes or str
            pt = type(payload)
            plen = len(payload) if pt is bytes or pt is str or pt is bytearray else 0
            self._total_bytes += plen
            self._buffer.append((opcode, payload))
            # Apply backpressure: message count OR byte count
            if (not self._paused
                    and (len(self._buffer) >= self._high_water
                         or self._total_bytes >= self._byte_high_water)
                    and self._protocol is not None):
                self._paused = True
                transport = self._protocol._transport
                if transport:
                    transport.pause_reading()

    async def get(self) -> tuple:
        """Get next frame — called from endpoint coroutine."""
        buf = self._buffer
        if buf:
            item = buf.popleft()  # O(1) instead of O(N) list.pop(0)
            pt = type(item[1])
            plen = len(item[1]) if pt is bytes or pt is str or pt is bytearray else 0
            self._total_bytes -= plen
            # Resume reading when BOTH message count AND byte count are below low water
            if (self._paused
                    and len(buf) <= self._low_water
                    and self._total_bytes <= self._byte_low_water
                    and self._protocol is not None):
                self._paused = False
                transport = self._protocol._transport
                if transport:
                    transport.resume_reading()
            return item
        fut = self._loop.create_future()
        self._waiter = fut
        return await fut


# C++ WebSocket — single-call handlers (v4 architecture)
# C++ does ALL frame processing; Python is only syntax + async dispatch.
try:
    from fastapi._fastapi_core import (
        # Single-call frame handlers (ring buffer + parse + decode + consume)
        ws_handle_direct as _ws_handle_direct,
        ws_echo_direct as _ws_handle_echo_direct,
        ws_handle_json_direct as _ws_handle_json_direct,
        # Frame building (send_text/send_bytes/send_json/close/ping)
        ws_build_frame_bytes as _ws_build_frame_bytes,
        ws_build_ping_frame as _ws_build_ping_frame,
        ws_build_close_frame_bytes as _ws_build_close_frame_bytes,
        ws_build_frames_batch as _ws_build_frames_batch,
        # Combined JSON serialize + frame build (single allocation)
        ws_build_json_frame as _ws_build_json_frame,
        # JSON parse/serialize (receive_json/send_json)
        ws_parse_json as _ws_parse_json,
        ws_serialize_json as _ws_serialize_json,
        # Ring buffer lifecycle
        ws_ring_buffer_create as _ws_ring_buffer_create,
        ws_ring_buffer_reset as _ws_ring_buffer_reset,
        # Direct socket echo v2 (exclusive FD writes, EAGAIN buffering)
        ws_echo_direct_fd_v2 as _ws_echo_direct_fd_v2,
        ws_flush_pending as _ws_flush_pending,
        # HTTP response helpers
        build_response_from_parts as _build_response_from_parts,
        build_chunked_frame as _build_chunked_frame,
        # Metrics (tracked in C++ to avoid per-frame Python attribute stores)
        ws_get_metrics as _ws_get_metrics,
        ws_update_send_metrics as _ws_update_send_metrics,
        # Direct channel feed (bypasses intermediate Python list)
        ws_handle_and_feed as _ws_handle_and_feed,
        # HTTP connection buffer (replaces Python bytearray + memmove)
        http_buf_create as _http_buf_create,
        http_buf_append as _http_buf_append,
        http_buf_get_view as _http_buf_get_view,
        http_buf_consume as _http_buf_consume,
        http_buf_clear as _http_buf_clear,
        http_buf_len as _http_buf_len,
        # Warm-up / eager initialization
        init_cached_refs as _init_cached_refs,
        prewarm_buffer_pool as _prewarm_buffer_pool,
    )
except ImportError as e:
    raise ImportError(
        f"C++ WebSocket module failed to load: {e}. "
        "Rebuild with: cd cpp_core/build && cmake .. && make -j$(nproc) && "
        "cp _fastapi_core.so ../../fastapi/_fc_tmp.so && "
        "mv ../../fastapi/_fc_tmp.so ../../fastapi/_fastapi_core.so"
    ) from e


# ── WebSocket connection state pool ───────────────────────────────────────────
class _WsConnectionPool:
    """Pool pre-allocated WS connection state capsules for reuse."""
    __slots__ = ('_capsules', '_max_size')

    def __init__(self, max_size: int = 64):
        self._capsules: list = []
        self._max_size = max_size

    def acquire(self):
        if self._capsules:
            cap = self._capsules.pop()
            if _ws_ring_buffer_reset is not None:
                _ws_ring_buffer_reset(cap)
            return cap
        if _ws_ring_buffer_create is not None:
            return _ws_ring_buffer_create()
        return None

    def release(self, capsule) -> None:
        if capsule is None:
            return
        if len(self._capsules) < self._max_size:
            if _ws_ring_buffer_reset is not None:
                _ws_ring_buffer_reset(capsule)
            self._capsules.append(capsule)
        # else: let GC reclaim it

_ws_pool = _WsConnectionPool() if _ws_ring_buffer_create is not None else None

# Pre-built WebSocket PING frame — reused forever (avoids rebuild per heartbeat)
_PING_FRAME: bytes | None = None
if _ws_build_ping_frame is not None:
    try:
        _PING_FRAME = _ws_build_ping_frame(None)
    except Exception:
        pass
if _PING_FRAME is None:
    # Python fallback: raw PING frame (opcode 0x89, no payload, no mask)
    _PING_FRAME = b"\x89\x00"

# ── WebSocket endpoint signature cache ────────────────────────────────────────
_ws_sig_cache: dict[int, str] = {}

def precompute_ws_signature(endpoint: Any) -> None:
    """Pre-compute WebSocket parameter name at route registration time.
    Called from applications.py add_websocket_route() to avoid
    inspect.signature() overhead on first WS connection."""
    ep_id = id(endpoint)
    if ep_id in _ws_sig_cache:
        return
    sig = inspect.signature(endpoint)
    ws_param = "websocket"
    for param_name, param in sig.parameters.items():
        ann = param.annotation
        if param_name == "websocket" or (
            ann is not inspect.Parameter.empty
            and getattr(ann, "__name__", "") == "WebSocket"
        ):
            ws_param = param_name
            break
    _ws_sig_cache[ep_id] = ws_param

# ── Pre-built responses (allocated once, reused forever) ─────────────────────

_500_RESP = (
    b"HTTP/1.1 500 Internal Server Error\r\n"
    b"content-type: application/json\r\n"
    b"content-length: 34\r\n"
    b"connection: keep-alive\r\n"
    b"\r\n"
    b'{"detail":"Internal Server Error"}'
)

# ── Type cache (initialized at import time) ──────────────────────────────────

try:
    from fastapi._core_bridge import InlineResult as _InlineResult
except ImportError:
    _InlineResult = None

# ── Pydantic validation (initialized at import time) ─────────────────────────

try:
    from fastapi.dependencies.utils import request_body_to_args as _request_body_to_args
except ImportError:
    _request_body_to_args = None


# ── WebSocket shared helpers ──────────────────────────────────────────────────

def _send_close_response(transport, payload: bytes) -> None:
    """Build and send close frame response. Single place for close logic."""
    code = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 1000
    if _ws_build_close_frame_bytes is not None:
        resp = _ws_build_close_frame_bytes(code)
    else:
        resp = CppWebSocket._build_frame_py(
            0x8, payload[:2] if len(payload) >= 2 else b"\x03\xe8")
    if transport and not transport.is_closing():
        transport.write(resp)


def _feed_frames(ws, transport, frames, ring_buf) -> bool:
    """Feed parsed frames to WebSocket channel. Returns True if close detected.

    Metrics are tracked in C++ (ws_ring_buffer.cpp) — no per-frame Python updates needed.
    """
    channel = ws._channel
    waiter = channel._waiter
    buf = channel._buffer
    for opcode, payload in frames:
        if opcode == 0x8:  # Close frame
            # Resolve graceful close waiter if pending (our close was acknowledged)
            cw = ws._close_waiter
            if cw is not None and not cw.done():
                cw.set_result(None)
            if waiter is not None and not waiter.done():
                channel._waiter = None
                waiter.set_result((0x8, payload))
            else:
                buf.append((0x8, payload))
            _send_close_response(transport, payload)
            if ring_buf is not None and _ws_ring_buffer_reset is not None:
                _ws_ring_buffer_reset(ring_buf)
            return True
        # Data frame — feed to channel directly (metrics tracked in C++)
        if waiter is not None and not waiter.done():
            channel._waiter = None
            waiter.set_result((opcode, payload))
            waiter = None  # only first frame resolves waiter
        else:
            buf.append((opcode, payload))
    return False


# ── Per-connection metrics ────────────────────────────────────────────────────

class _WsMetrics:
    """Lightweight WebSocket connection metrics."""
    __slots__ = (
        'messages_sent', 'messages_received',
        'bytes_sent', 'bytes_received',
        'errors', 'connected_at', 'last_activity',
    )

    def __init__(self):
        self.messages_sent = 0
        self.messages_received = 0
        self.bytes_sent = 0
        self.bytes_received = 0
        self.errors = 0
        now = time.monotonic()
        self.connected_at = now
        self.last_activity = now


# ── Server-level WebSocket metrics ───────────────────────────────────────────

class _WsServerMetrics:
    """Aggregate metrics across all WebSocket connections."""
    __slots__ = ('active_connections', 'total_connections', 'total_messages')

    def __init__(self):
        self.active_connections = 0
        self.total_connections = 0
        self.total_messages = 0

_server_ws_metrics = _WsServerMetrics()


# ── WebSocket wrapper for Python endpoint access ────────────────────────────

class CppWebSocket:
    """WebSocket connection wrapper for asyncio transport.

    Provides send/receive API compatible with FastAPI/Starlette WebSocket endpoints.
    Frame parsing and building done via C++ ws_frame_parser (RFC 6455).
    """

    __slots__ = (
        "_transport", "_loop", "_channel", "_closed", "_close_code",
        "client_state", "application_state", "scope", "path_params",
        "_corked", "_cork_buf",
        "_protocol", "_echo_detect_count", "_last_received",
        "_last_received_json",
        "_pending_sends", "_flush_scheduled",
        "_close_waiter", "_metrics",
    )

    def __init__(
        self,
        transport: asyncio.Transport,
        loop: asyncio.AbstractEventLoop,
        path: str = "/",
        path_params: dict | None = None,
        headers: list | None = None,
        query_string: bytes = b"",
    ) -> None:
        self._transport = transport
        self._loop = loop
        self._channel = _WsFastChannel(loop)  # protocol set later via _channel._protocol
        self._closed = False
        self._close_code = 1000
        self._corked = False
        self._cork_buf: list[tuple[int, bytes]] = []
        # Auto-cork: coalesce writes within same event loop tick, batch frame building
        self._pending_sends: list[tuple[int, bytes]] = []
        self._flush_scheduled = False
        # Echo auto-detection
        self._protocol = None  # back-ref to CppHttpProtocol, set after creation
        self._echo_detect_count = 0
        self._last_received = None  # track last received for echo detection
        self._last_received_json = None  # track last received JSON for echo detection
        # Graceful close
        self._close_waiter: asyncio.Future | None = None
        # Per-connection metrics
        self._metrics = _WsMetrics()
        # Starlette-compatible state tracking
        self.client_state = WebSocketState.CONNECTED   # 101 already sent by C++
        self.application_state = WebSocketState.CONNECTING
        self.path_params = path_params or {}
        self.scope = {
            "type": "websocket",
            "path": path,
            "path_params": self.path_params,
            "headers": headers if headers is not None else [],
            "query_string": query_string or b"",
            "scheme": "ws",
            "root_path": "",
            "state": {},
        }

    # ── Lifecycle ─────────────────────────────────────────────────────

    async def accept(self, subprotocol: str | None = None, headers: list | None = None) -> None:
        """Accept the WebSocket connection (upgrade already sent by C++).

        Note: The 101 upgrade response was already sent by C++ during HTTP parsing.
        Subprotocol is stored in scope for endpoint introspection.
        Additional response headers are not supported in app.run() mode since
        the upgrade response has already been sent.
        """
        self.application_state = WebSocketState.CONNECTED
        if subprotocol is not None:
            self.scope["subprotocol"] = subprotocol

    async def close(self, code: int = 1000, reason: str | None = None) -> None:
        """Send close frame, wait for peer close response, then TCP close."""
        if self._closed:
            return
        self._closed = True
        self._close_code = code
        self.application_state = WebSocketState.DISCONNECTED
        if self._corked:
            self._flush_cork()
        # Flush any pending auto-corked writes
        if self._pending_sends:
            self._flush_pending()
        frame = _ws_build_close_frame_bytes(code) if _ws_build_close_frame_bytes else self._build_frame_py(0x8, struct.pack("!H", code))
        try:
            self._transport.write(frame)
        except Exception:
            logger.debug("WS close frame write failed", exc_info=True)
            return
        # Wait for peer close response (graceful handshake)
        try:
            close_wait = self._loop.create_future()
            self._close_waiter = close_wait
            await asyncio.wait_for(close_wait, timeout=5.0)
        except (asyncio.TimeoutError, Exception):
            pass
        finally:
            self._close_waiter = None

    # ── Write helpers ─────────────────────────────────────────────────

    def _queue_send(self, opcode: int, payload: bytes) -> None:
        """Queue a send for batch building: coalesces within same event loop tick."""
        transport = self._transport
        if transport is None or transport.is_closing():
            return
        self._pending_sends.append((opcode, payload))
        if not self._flush_scheduled:
            self._flush_scheduled = True
            self._loop.call_soon(self._flush_pending)

    def _flush_pending(self) -> None:
        """Batch-build and flush all pending frames in a single write."""
        self._flush_scheduled = False
        pending = self._pending_sends
        if not pending:
            return
        transport = self._transport
        if transport is None or transport.is_closing():
            pending.clear()
            return
        if len(pending) == 1:
            opcode, payload = pending[0]
            frame = _ws_build_frame_bytes(opcode, payload) if _ws_build_frame_bytes else self._build_frame_py(opcode, payload)
            transport.write(frame)
        else:
            # Batch-build all frames in single C++ call (one allocation, one memcpy)
            if _ws_build_frames_batch is not None:
                combined = _ws_build_frames_batch(pending)
                transport.write(combined)
            else:
                for opcode, payload in pending:
                    transport.write(self._build_frame_py(opcode, payload))
        pending.clear()

    def _write_frame(self, frame: bytes) -> None:
        """Write a pre-built WebSocket frame directly to transport.

        Used by WebSocket group broadcasts (_ws_groups.py) to send
        pre-built frames without re-encoding.
        """
        transport = self._transport
        if transport is not None and not transport.is_closing():
            transport.write(frame)

    # ── Send methods ──────────────────────────────────────────────────

    def send_text(self, data: str) -> _NoopAwaitable:
        """Send a text message. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        # Echo auto-detection: if sending exactly what was received, switch handler
        # Single match triggers — false positive cost is zero (echo handler is correct)
        if _ws_handle_echo_direct is not None and self._echo_detect_count >= 0:
            if self._last_received is not None and data == self._last_received:
                self._echo_detect_count += 1
                if self._echo_detect_count >= 1:
                    proto = self._protocol
                    if proto is not None:
                        # Use direct socket echo v2 when FD available (exclusive
                        # writes with EAGAIN buffering — no interleaving)
                        if proto._ws_fd >= 0 and _ws_echo_direct_fd_v2 is not None:
                            proto._ws_handler = proto._handle_ws_frames_echo_fd
                        else:
                            proto._ws_handler = proto._handle_ws_frames_echo
                    self._echo_detect_count = -1  # stop detecting
            else:
                self._echo_detect_count = 0  # non-match resets counter
            self._last_received = None
        payload = data if isinstance(data, bytes) else data.encode("utf-8")
        # Send metrics tracked in C++ (avoids 3 Python attribute stores per message)
        if self._corked:
            self._cork_buf.append((0x1, payload))
            return _NOOP
        self._queue_send(0x1, payload)
        return _NOOP

    def send_bytes(self, data: bytes) -> _NoopAwaitable:
        """Send a binary message. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        if self._corked:
            self._cork_buf.append((0x2, data))
            return _NOOP
        self._queue_send(0x2, data)
        return _NOOP

    def send_json(self, data: Any, mode: str = "text") -> _NoopAwaitable:
        """Send JSON data. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        # JSON echo detection: if sending the exact same object received via receive_json,
        # switch to raw text echo handler (bypasses JSON parse+serialize entirely).
        if self._echo_detect_count >= 0 and self._last_received_json is not None:
            if data is self._last_received_json:
                self._echo_detect_count += 1
                if self._echo_detect_count >= 1:
                    proto = self._protocol
                    if proto is not None:
                        if proto._ws_fd >= 0 and _ws_echo_direct_fd_v2 is not None:
                            proto._ws_handler = proto._handle_ws_frames_echo_fd
                        else:
                            proto._ws_handler = proto._handle_ws_frames_echo
                    self._echo_detect_count = -1
            else:
                self._echo_detect_count = 0
            self._last_received_json = None
        opcode = 0x1 if mode == "text" else 0x2
        if _ws_build_json_frame is not None:
            # Combined serialize + frame build: single C++ call, single allocation
            frame = _ws_build_json_frame(data, opcode)
            if self._corked:
                # Store as (None, raw_frame) to mark as pre-built — skip re-framing in _flush_cork
                self._cork_buf.append((None, frame))
                return _NOOP
            # Write pre-built frame directly (bypass _queue_send batch building)
            transport = self._transport
            if transport is not None and not transport.is_closing():
                transport.write(frame)
        elif _ws_serialize_json is not None:
            json_bytes = _ws_serialize_json(data)
            if self._corked:
                self._cork_buf.append((opcode, json_bytes))
                return _NOOP
            self._queue_send(opcode, json_bytes)
        else:
            from fastapi._json_utils import json_dumps
            payload = json_dumps(data)
            if self._corked:
                self._cork_buf.append((opcode, payload))
                return _NOOP
            self._queue_send(opcode, payload)
        return _NOOP

    # ── Receive methods ───────────────────────────────────────────────

    def _parse_close_code(self, payload: bytes) -> int:
        """Extract close code from close frame payload (RFC 6455 §5.5.1)."""
        if len(payload) >= 2:
            return int.from_bytes(payload[:2], "big")
        return 1000

    async def receive_text(self) -> str:
        """Receive a text message."""
        opcode, payload = await self._channel.get()
        if opcode == 0x8:  # close
            self._closed = True
            self.client_state = WebSocketState.DISCONNECTED
            raise WebSocketDisconnect(code=self._parse_close_code(payload))
        result = payload if isinstance(payload, str) else payload.decode("utf-8")
        self._last_received = result
        return result

    async def receive_bytes(self) -> bytes:
        """Receive a binary message."""
        opcode, payload = await self._channel.get()
        if opcode == 0x8:
            self._closed = True
            self.client_state = WebSocketState.DISCONNECTED
            raise WebSocketDisconnect(code=self._parse_close_code(payload))
        return payload

    async def receive_json(self, mode: str = "text") -> Any:
        """Receive and parse JSON data."""
        # Switch to JSON frame handler on first call (parses JSON in C++)
        if _ws_handle_json_direct is not None and self._protocol is not None:
            proto = self._protocol
            if proto._ws_handler is not proto._handle_ws_frames_json:
                proto._ws_handler = proto._handle_ws_frames_json
        opcode, payload = await self._channel.get()
        if opcode == 0x8:
            self._closed = True
            self.client_state = WebSocketState.DISCONNECTED
            raise WebSocketDisconnect(code=self._parse_close_code(payload))
        # If payload is already a parsed Python object (from _ws_parse_frames_json),
        # return it directly. Otherwise parse it.
        if isinstance(payload, (dict, list, int, float, bool)) or payload is None:
            self._last_received_json = payload  # save ref for echo detection
            return payload
        if isinstance(payload, str):
            if _ws_parse_json is not None:
                result = _ws_parse_json(payload)
                self._last_received_json = result
                return result
            from fastapi._json_utils import json_loads
            result = json_loads(payload)
            self._last_received_json = result
            return result
        # bytes
        if _ws_parse_json is not None:
            result = _ws_parse_json(payload)
            self._last_received_json = result
            return result
        from fastapi._json_utils import json_loads
        result = json_loads(payload)
        self._last_received_json = result
        return result

    # ── Dict-based ASGI protocol methods ──────────────────────────────

    async def receive(self) -> dict:
        """Receive an ASGI-style WebSocket message dict."""
        opcode, payload = await self._channel.get()
        if opcode == 0x8:
            self._closed = True
            self.client_state = WebSocketState.DISCONNECTED
            return {"type": "websocket.disconnect", "code": self._parse_close_code(payload)}
        elif opcode == 0x1:
            return {"type": "websocket.receive", "text": payload.decode("utf-8")}
        else:
            return {"type": "websocket.receive", "bytes": payload}

    async def send(self, message: dict) -> None:
        """Send an ASGI-style WebSocket message dict."""
        msg_type = message.get("type", "")
        if msg_type == "websocket.accept":
            await self.accept(
                subprotocol=message.get("subprotocol"),
                headers=message.get("headers"),
            )
        elif msg_type == "websocket.send":
            if "text" in message:
                await self.send_text(message["text"])
            elif "bytes" in message:
                await self.send_bytes(message["bytes"])
        elif msg_type == "websocket.close":
            await self.close(code=message.get("code", 1000))

    # ── Async iterators ───────────────────────────────────────────────

    async def iter_text(self):
        """Async iterator yielding text messages until disconnect."""
        try:
            while True:
                yield await self.receive_text()
        except WebSocketDisconnect:
            pass

    async def iter_bytes(self):
        """Async iterator yielding binary messages until disconnect."""
        try:
            while True:
                yield await self.receive_bytes()
        except WebSocketDisconnect:
            pass

    async def iter_json(self):
        """Async iterator yielding parsed JSON messages until disconnect."""
        try:
            while True:
                yield await self.receive_json()
        except WebSocketDisconnect:
            pass

    async def iter_binary_chunks(self, chunk_size: int = 65536):
        """Async iterator yielding binary message chunks as they arrive.
        Avoids buffering entire large binary messages in memory."""
        assembler = bytearray()
        try:
            while True:
                opcode, payload = await self._channel.get()
                if opcode == 0x8:
                    self._closed = True
                    self.client_state = WebSocketState.DISCONNECTED
                    break
                if isinstance(payload, (bytes, bytearray)):
                    assembler.extend(payload)
                    while len(assembler) >= chunk_size:
                        yield bytes(assembler[:chunk_size])
                        del assembler[:chunk_size]
        except WebSocketDisconnect:
            pass
        if assembler:
            yield bytes(assembler)

    # ── Write batching (cork/uncork) ─────────────────────────────────

    def cork(self) -> None:
        """Start buffering outgoing frames for batch write.
        Also sets TCP_CORK to batch TCP segments (Linux only)."""
        self._corked = True
        proto = self._protocol
        if proto is not None and proto._ws_fd >= 0 and sys.platform == "linux":
            try:
                sock = proto._transport.get_extra_info("socket")
                if sock is not None:
                    sock.setsockopt(socket.IPPROTO_TCP, 3, 1)  # TCP_CORK = 3
            except (OSError, AttributeError):
                pass

    def uncork(self) -> None:
        """Flush buffered frames as a single write and stop buffering.
        Also clears TCP_CORK to flush batched TCP segments."""
        self._flush_cork()
        self._corked = False
        proto = self._protocol
        if proto is not None and proto._ws_fd >= 0 and sys.platform == "linux":
            try:
                sock = proto._transport.get_extra_info("socket")
                if sock is not None:
                    sock.setsockopt(socket.IPPROTO_TCP, 3, 0)  # TCP_CORK = 3
            except (OSError, AttributeError):
                pass

    def _flush_cork(self) -> None:
        """Flush corked frames."""
        if not self._cork_buf:
            return
        transport = self._transport
        if transport is None or transport.is_closing():
            self._cork_buf.clear()
            return
        # Separate pre-built frames (opcode=None) from raw payloads needing framing
        raw_entries = []
        for opcode, payload in self._cork_buf:
            if opcode is None:
                # Pre-built frame — flush any pending raw entries first, then write directly
                if raw_entries:
                    if _ws_build_frames_batch is not None:
                        transport.write(_ws_build_frames_batch(raw_entries))
                    else:
                        for op, pl in raw_entries:
                            transport.write(self._build_frame_py(op, pl))
                    raw_entries = []
                transport.write(payload)
            else:
                raw_entries.append((opcode, payload))
        # Flush remaining raw entries
        if raw_entries:
            if _ws_build_frames_batch is not None:
                transport.write(_ws_build_frames_batch(raw_entries))
            else:
                for op, pl in raw_entries:
                    transport.write(self._build_frame_py(op, pl))
        self._cork_buf.clear()

    # ── Metrics & Configuration ────────────────────────────────────────

    @property
    def metrics(self) -> _WsMetrics:
        """Per-connection WebSocket metrics."""
        return self._metrics

    def disable_echo_detection(self) -> None:
        """Disable echo auto-detection for non-echo endpoints."""
        self._echo_detect_count = -1

    # ── Internal ──────────────────────────────────────────────────────

    def feed_frame(self, opcode: int, payload: bytes) -> None:
        """Feed a parsed frame from the protocol handler."""
        self._channel.feed(opcode, payload)

    @staticmethod
    def _build_frame(opcode: int, payload: bytes) -> bytes:
        """Build an unmasked server→client WebSocket frame (C++ fast path)."""
        if _ws_build_frame_bytes is not None:
            return _ws_build_frame_bytes(opcode, payload)
        return CppWebSocket._build_frame_py(opcode, payload)

    @staticmethod
    def _build_frame_py(opcode: int, payload: bytes) -> bytes:
        """Build an unmasked server→client WebSocket frame (Python fallback)."""
        header = bytearray()
        header.append(0x80 | opcode)  # FIN + opcode
        plen = len(payload)
        if plen < 126:
            header.append(plen)
        elif plen <= 0xFFFF:
            header.append(126)
            header.extend(struct.pack("!H", plen))
        else:
            header.append(127)
            header.extend(struct.pack("!Q", plen))
        return bytes(header) + payload


# ── OPT-14: Protocol Object Pool ──────────────────────────────────────────
# Reuse CppHttpProtocol instances to avoid per-connection __init__ overhead
# and reduce GC pressure under high connection rates. Pool returns protocol
# objects to a free list on connection_lost(), reuses on next connection.

class _ProtocolPool:
    """Fixed-size pool of CppHttpProtocol objects for reuse."""
    __slots__ = ("_pool", "_max_size")

    def __init__(self, max_size: int = 10000):
        self._pool: list = []
        self._max_size = max_size

    def acquire(self, core_app: Any, loop: "asyncio.AbstractEventLoop",
                ka_timeout: float, connections_set: set | None) -> "CppHttpProtocol":
        if self._pool:
            proto = self._pool.pop()
            proto._reinit(core_app, loop, ka_timeout, connections_set)
            return proto
        return CppHttpProtocol(core_app, loop, ka_timeout, connections_set)

    def release(self, proto: "CppHttpProtocol") -> None:
        if len(self._pool) < self._max_size:
            self._pool.append(proto)

    def trim(self, target_size: int) -> int:
        """Trim pool to target_size, returning number of protocols freed."""
        current = len(self._pool)
        if current <= target_size:
            return 0
        del self._pool[target_size:]
        return current - target_size

_protocol_pool = _ProtocolPool()

# Connection limit — no hardcoded cap by default (like uWebSockets, Fiber, Bun).
# OS-level fd limits are the real constraint. Set FASTAPI_MAX_CONNECTIONS to enforce.
_MAX_CONNECTIONS = int(os.environ.get("FASTAPI_MAX_CONNECTIONS", "0"))  # 0 = unlimited
# At 80% capacity (if limit set), force Connection: close to cycle connections
_PRESSURE_THRESHOLD = _MAX_CONNECTIONS * 4 // 5 if _MAX_CONNECTIONS > 0 else 0


class _RejectProtocol(asyncio.Protocol):
    """Instantly respond 503 and close when server is at capacity."""
    __slots__ = ()

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        transport.write(  # type: ignore[union-attr]
            b"HTTP/1.1 503 Service Unavailable\r\n"
            b"Content-Length: 0\r\n"
            b"Connection: close\r\n\r\n"
        )
        transport.close()  # type: ignore[union-attr]


# ── Coroutine driver — properly forwards exceptions via throw() ──────────────

async def _drive_coro(coro: Any, first_yield: Any) -> Any:
    """Drive a partially-consumed coroutine to completion.

    C++ drove the first step via PyIter_Send; this finishes the rest.
    Mimics asyncio Task.__step: on await exception, throws it back into
    the coroutine so user-defined handlers (try/except) work correctly.

    Fast-path: most async endpoints have exactly 1 await (e.g. a DB query).
    We inline the single-yield case to avoid the loop + extra try/except.
    """
    # Single-yield fast-path (most common case)
    result = await first_yield
    try:
        next_yield = coro.send(result)
    except StopIteration as e:
        return e.value
    # Multi-yield: fall through to loop
    while True:
        try:
            next_yield._asyncio_future_blocking = False
        except AttributeError:
            pass
        try:
            result = await next_yield
        except BaseException as exc:
            try:
                next_yield = coro.throw(type(exc), exc, exc.__traceback__)
            except StopIteration as e:
                return e.value
        else:
            try:
                next_yield = coro.send(result)
            except StopIteration as e:
                return e.value


class CppHttpProtocol(asyncio.Protocol):
    """Per-connection HTTP/1.1 protocol with WebSocket upgrade support.

    Processing model:
      data_received() → C++ handle_http(buf, transport) handles everything inline:
        - HTTP parse, route match, param extraction, DI resolution
        - CORS preflight/headers, trusted host check, exception handlers
        - JSON serialization, gzip/brotli compression
        - Sync endpoints complete entirely in C++ — zero Python overhead
        - Async endpoints: C++ returns packed context, Python awaits + writes
        - WebSocket: C++ sends 101 upgrade, Python handles frame lifecycle
        - Pydantic routes: C++ returns InlineResult for validation
    """

    __slots__ = ("_core", "_transport", "_http_buf", "_ka_deadline", "_ka_timeout", "_loop", "_wr_paused", "_ws", "_ws_handler", "_ws_ring_buf", "_ws_fd", "_ws_ping_handle", "_ws_pong_received", "_ws_task", "_connections_set", "_pending_tasks", "_active_count", "_sock_fd")

    def __init__(self, core_app: Any, loop: asyncio.AbstractEventLoop,
                 keep_alive_timeout: float = 15.0,
                 connections_set: set | None = None) -> None:
        self._core = core_app
        self._loop = loop
        self._ka_timeout = keep_alive_timeout
        self._connections_set = connections_set
        self._active_count: list | None = None  # mutable int ref for fast connection counting
        self._transport: asyncio.Transport | None = None
        # C++ HTTP connection buffer (O(1) consume vs O(N) memmove)
        self._http_buf = _http_buf_create()
        self._ka_deadline: float = 0.0
        self._wr_paused = False
        self._ws_handler = None  # Will be set to echo/json/normal handler after upgrade
        self._ws: CppWebSocket | None = None
        self._ws_ring_buf = None  # C++ connection state capsule (created on WebSocket upgrade)
        self._ws_fd = -1  # Raw socket FD for direct C++ writes (bypassing asyncio)
        self._ws_ping_handle: asyncio.TimerHandle | None = None
        self._ws_pong_received = True
        self._ws_task: asyncio.Task | None = None  # TS-2: tracked for cancellation
        self._pending_tasks: set | None = None  # Lazy — only allocated for async endpoints
        self._sock_fd = -1  # Raw socket FD for direct C++ HTTP writes

    def _reinit(self, core_app: Any, loop: "asyncio.AbstractEventLoop",
                ka_timeout: float, connections_set: set | None) -> None:
        """Reset protocol for reuse from pool (OPT-14). Avoids __init__ overhead."""
        self._core = core_app
        self._loop = loop
        self._ka_timeout = ka_timeout
        self._connections_set = connections_set
        self._active_count = None
        self._transport = None
        _http_buf_clear(self._http_buf)  # reuse existing C++ buffer
        self._ka_deadline = 0.0
        self._wr_paused = False
        self._ws_handler = None
        self._ws = None
        self._ws_ring_buf = None
        self._ws_fd = -1
        self._sock_fd = -1
        self._ws_ping_handle = None
        self._ws_pong_received = True
        self._ws_task = None
        if self._pending_tasks is not None:
            self._pending_tasks.clear()  # reuse existing set's backing store

    # ── Connection lifecycle ─────────────────────────────────────────────

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self._transport = transport  # type: ignore[assignment]
        sock: socket.socket | None = transport.get_extra_info("socket")  # type: ignore[union-attr]
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if _HAS_QUICKACK:
                try:
                    sock.setsockopt(socket.IPPROTO_TCP, _TCP_QUICKACK, 1)
                except OSError:
                    pass
            try:
                self._sock_fd = sock.fileno()
            except (OSError, AttributeError):
                self._sock_fd = -1
        # Reduced from 1MB/256KB — saves ~7.5GB potential buffer at 10K connections
        transport.set_write_buffer_limits(high=131072, low=32768)  # type: ignore[union-attr]
        peername = transport.get_extra_info("peername")
        if peername:
            self._core.set_client_ip(peername[0])
        self._ka_reset()

    def connection_lost(self, exc: Exception | None) -> None:
        # Cancel all pending async HTTP tasks to prevent writes to stale transport
        if self._pending_tasks:
            for t in self._pending_tasks:
                if not t.done():
                    t.cancel()
            self._pending_tasks.clear()
        self._ka_cancel()
        self._stop_ws_heartbeat()
        # TS-2: Cancel tracked WebSocket task to prevent hanging
        if self._ws_task is not None and not self._ws_task.done():
            self._ws_task.cancel()
            self._ws_task = None
        if self._ws:
            _server_ws_metrics.active_connections = max(0, _server_ws_metrics.active_connections - 1)
        if self._ws and not self._ws._closed:
            self._ws._closed = True
            self._ws.feed_frame(0x8, b"")  # signal close to waiting receives
        _http_buf_clear(self._http_buf)
        # Return ring buffer to pool for reuse (or reset and drop)
        if self._ws_ring_buf is not None:
            if _ws_pool is not None:
                _ws_pool.release(self._ws_ring_buf)
            elif _ws_ring_buffer_reset is not None:
                _ws_ring_buffer_reset(self._ws_ring_buf)
            self._ws_ring_buf = None
        self._ws_fd = -1
        self._sock_fd = -1
        self._transport = None
        # Remove from active connections set (graceful shutdown tracking)
        if self._connections_set is not None:
            self._connections_set.discard(self)
        # Decrement fast connection counter + relieve pressure
        if self._active_count is not None:
            self._active_count[0] -= 1
            if self._active_count[0] <= _PRESSURE_THRESHOLD and self._core.force_close:
                self._core.force_close = 0
        # OPT-14: Return protocol to pool for reuse
        _protocol_pool.release(self)

    # ── Data handling — the hot path ─────────────────────────────────────

    def data_received(self, data: bytes) -> None:
        # WebSocket frame handling (after upgrade) — dispatch to mode-specific handler
        if self._ws is not None:
            self._ws_pong_received = True
            self._ws_handler(data)
            return

        # Back-pressure: buffer data but don't process until writes resume
        if self._wr_paused:
            if not _http_buf_append(self._http_buf, data):
                transport = self._transport
                if transport and not transport.is_closing():
                    transport.write(b"HTTP/1.1 413 Payload Too Large\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                    transport.close()
                _http_buf_clear(self._http_buf)
            return

        transport = self._transport
        if not transport or transport.is_closing():
            return

        # ── Append + parse loop: 6 individual C++ calls ──────────────────
        http_buf = self._http_buf
        if not _http_buf_append(http_buf, data):
            transport.write(b"HTTP/1.1 413 Payload Too Large\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
            transport.close()
            _http_buf_clear(http_buf)
            return

        # Pass capsule directly to C++ — avoids PyMemoryView_FromMemory per call
        if _http_buf_len(http_buf) == 0:
            return

        core = self._core
        offset = 0
        IR = _InlineResult
        sock_fd = self._sock_fd
        now = self._loop.time()

        while True:
            result = core.handle_http(http_buf, transport, offset, sock_fd)

            # ── Fast-path: zero-alloc return protocol ──────────────────
            # True  = sync endpoint handled (response already written)
            # None  = need more data (incomplete HTTP request)
            # False = parse error (400 already sent)
            # tuple = async/WS/Pydantic dispatch (rare path)
            if result is True:
                offset += core.last_consumed
                continue

            if result is None:
                break  # need more data

            if result is False:
                # Parse error (400 sent by C++)
                transport.close()
                _http_buf_clear(http_buf)
                return

            # ── Async / WS / Pydantic dispatch (tuple return) ─────────
            consumed, result_obj = result
            offset += consumed

            if isinstance(result_obj, tuple):
                tag = result_obj[0]
                if tag == "ws":
                    rlen = len(result_obj)
                    ws_path = result_obj[3] if rlen >= 4 else "/"
                    ws_headers = result_obj[4] if rlen >= 5 else []
                    ws_query_string = result_obj[5] if rlen >= 6 else b""
                    endpoint, path_params = result_obj[1], result_obj[2]
                    if endpoint is None:
                        if transport and not transport.is_closing():
                            transport.close()
                        _http_buf_clear(http_buf)
                        return
                    # Consume bytes processed so far before switching to WS
                    _http_buf_consume(http_buf, offset)
                    ws = CppWebSocket(
                        transport, self._loop,
                        path=ws_path, path_params=path_params,
                        headers=ws_headers, query_string=ws_query_string,
                    )
                    ws._protocol = self
                    ws._channel._protocol = self
                    self._ws = ws
                    _server_ws_metrics.active_connections += 1
                    _server_ws_metrics.total_connections += 1
                    self._ka_cancel()
                    self._ws_fd = -1
                    try:
                        sock = transport.get_extra_info("socket")
                        if sock is not None:
                            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                            self._ws_fd = sock.fileno()
                    except (OSError, AttributeError):
                        pass
                    transport.set_write_buffer_limits(high=2097152, low=524288)
                    if _ws_pool is not None:
                        self._ws_ring_buf = _ws_pool.acquire()
                    elif _ws_ring_buffer_create is not None:
                        self._ws_ring_buf = _ws_ring_buffer_create()
                    self._ws_handler = self._handle_ws_frames
                    self._start_ws_heartbeat()
                    self._ws_task = self._loop.create_task(
                        self._handle_websocket(endpoint, path_params))
                    # Remaining buffer data (if any) is WebSocket frames
                    remaining_mv = _http_buf_get_view(http_buf)
                    remaining = bytes(remaining_mv) if remaining_mv is not None else b""
                    if remaining_mv is not None:
                        remaining_mv.release()
                    _http_buf_clear(http_buf)
                    if remaining:
                        self._ws_handler(remaining)
                    return  # connection is now WebSocket

                elif tag == "async":
                    _, coro, first_yield, status_code, keep_alive = result_obj
                    first_yield._asyncio_future_blocking = False
                    task = self._loop.create_task(
                        self._handle_async(coro, first_yield, status_code, keep_alive))
                    pt = self._pending_tasks
                    if pt is None:
                        pt = self._pending_tasks = set()
                    task.add_done_callback(pt.discard)
                    pt.add(task)

                elif tag == "async_di":
                    _, di_coro, first_yield, endpoint, kwargs, sc, ka = result_obj
                    first_yield._asyncio_future_blocking = False
                    task = self._loop.create_task(
                        self._handle_async_di(di_coro, first_yield, endpoint, kwargs, sc, ka))
                    pt = self._pending_tasks
                    if pt is None:
                        pt = self._pending_tasks = set()
                    task.add_done_callback(pt.discard)
                    pt.add(task)

            elif IR and type(result_obj) is IR:
                task = self._loop.create_task(self._handle_pydantic(result_obj))
                pt = self._pending_tasks
                if pt is None:
                    pt = self._pending_tasks = set()
                task.add_done_callback(pt.discard)
                pt.add(task)

        if offset > 0:
            _http_buf_consume(http_buf, offset)
        if _http_buf_len(http_buf) == 0:
            self._ka_reset(now)

    # ── WebSocket frame handling ─────────────────────────────────────────

    def _handle_ws_frames(self, data: bytes) -> None:
        """Normal frame handler — single C++ call parses + feeds directly to channel."""
        ws = self._ws
        if not ws:
            return
        try:
            channel = ws._channel
            result = _ws_handle_and_feed(
                self._ws_ring_buf, data, channel._waiter, channel._buffer)
            if result is None:
                return
            pong_bytes, close_payload, frames_fed = result
            # Clear waiter if it was resolved by C++
            if frames_fed > 0 and channel._waiter is not None:
                w = channel._waiter
                if w.done():
                    channel._waiter = None
            transport = self._transport
            if pong_bytes is not None and transport and not transport.is_closing():
                transport.write(pong_bytes)
            if close_payload is not None:
                _send_close_response(transport, close_payload)
                if self._ws_ring_buf is not None and _ws_ring_buffer_reset is not None:
                    _ws_ring_buffer_reset(self._ws_ring_buf)
            # Synchronous cork: flush any queued sends immediately (no call_soon latency)
            if ws._pending_sends:
                ws._flush_pending()
        except Exception as e:
            import traceback
            print(f"[WS-ERROR] _handle_ws_frames: {e}", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
            if self._transport and not self._transport.is_closing():
                self._transport.close()

    def _handle_ws_frames_echo(self, data: bytes) -> None:
        """Echo fast path — single C++ call does ring + parse + echo + consume."""
        transport = self._transport
        result = _ws_handle_echo_direct(self._ws_ring_buf, data)
        if result is None:
            return
        echo_bytes, close_payload = result
        if echo_bytes is not None and transport and not transport.is_closing():
            transport.write(echo_bytes)
        if close_payload is not None:
            ws = self._ws
            if ws:
                ws.feed_frame(0x8, close_payload)
                _send_close_response(transport, close_payload)

    def _handle_ws_frames_echo_fd(self, data: bytes) -> None:
        """Echo via direct socket write — bypasses asyncio transport entirely.
        On EAGAIN, buffers unsent data for retry via _flush_ws_echo_pending."""
        status = _ws_echo_direct_fd_v2(self._ws_ring_buf, data, self._ws_fd)
        if status == 0:
            return  # Happy path — echoed via direct socket
        if status == -1:
            # EAGAIN — data buffered in C++, schedule retry
            self._loop.call_soon(self._flush_ws_echo_pending)
        elif isinstance(status, tuple) and status[0] == 2:
            ws = self._ws
            if ws:
                ws.feed_frame(0x8, status[1])
                _send_close_response(self._transport, status[1])

    def _flush_ws_echo_pending(self) -> None:
        """Retry sending buffered WS echo data via direct socket."""
        if self._ws_fd < 0:
            return
        if _ws_flush_pending(self._ws_ring_buf, self._ws_fd) == -1:
            self._loop.call_soon(self._flush_ws_echo_pending)  # retry next tick

    def _handle_ws_frames_json(self, data: bytes) -> None:
        """JSON mode — single C++ call does ring + parse + unmask + JSON decode."""
        ws = self._ws
        if not ws:
            return
        try:
            result = _ws_handle_json_direct(self._ws_ring_buf, data)
            if result is None:
                return
            frames, pong_bytes = result
            transport = self._transport
            if pong_bytes is not None and transport and not transport.is_closing():
                transport.write(pong_bytes)
            _feed_frames(ws, transport, frames, self._ws_ring_buf)
            # Synchronous cork: flush any queued sends immediately (no call_soon latency)
            if ws._pending_sends:
                ws._flush_pending()
        except Exception as e:
            import traceback
            print(f"[WS-ERROR] _handle_ws_frames_json: {e}", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
            if self._transport and not self._transport.is_closing():
                self._transport.close()

    # ── Back-pressure (write flow control) ───────────────────────────────

    def pause_writing(self) -> None:
        self._wr_paused = True

    def resume_writing(self) -> None:
        self._wr_paused = False
        if _http_buf_len(self._http_buf) > 0 and self._transport and not self._transport.is_closing():
            self._loop.call_soon(self._drain_buf)

    def _drain_buf(self) -> None:
        if self._wr_paused or _http_buf_len(self._http_buf) == 0:
            return
        self.data_received(b"")

    # ── WebSocket heartbeat (PING/PONG) ───────────────────────────────────

    def _start_ws_heartbeat(self, interval: float = 30.0) -> None:
        """Start periodic PING frames to detect dead connections."""
        self._ws_pong_received = True
        self._ws_ping_handle = self._loop.call_later(interval, self._send_ws_ping, interval)

    def _send_ws_ping(self, interval: float) -> None:
        """Send PING frame and check if previous PONG was received."""
        self._ws_ping_handle = None
        ws = self._ws
        if not ws or ws._closed:
            return
        transport = self._transport
        if not transport or transport.is_closing():
            return

        if not self._ws_pong_received:
            # No PONG received since last PING — connection is dead
            ws._closed = True
            ws.feed_frame(0x8, b"")
            transport.close()
            return

        self._ws_pong_received = False
        transport.write(_PING_FRAME)  # pre-built at module init

        # Schedule next ping
        self._ws_ping_handle = self._loop.call_later(interval, self._send_ws_ping, interval)

    def _stop_ws_heartbeat(self) -> None:
        """Cancel heartbeat timer."""
        h = self._ws_ping_handle
        if h is not None:
            h.cancel()
            self._ws_ping_handle = None

    # ── Starlette Response helper ─────────────────────────────────────────

    def _write_response_obj(self, resp_obj: Any, keep_alive: bool) -> None:
        """Write a Starlette Response object (HTMLResponse, etc.) to transport."""
        body = resp_obj.body
        if not isinstance(body, bytes):
            body = body.encode("utf-8")
        sc = resp_obj.status_code
        headers_list = [(k, v) for k, v in resp_obj.headers.items()]
        self._transport.write(
            _build_response_from_parts(sc, headers_list, body, keep_alive)
        )

    def _write_error(self, exc: Exception, keep_alive: bool) -> None:
        """Write error response — handles HTTPException with proper status code."""
        if self._transport and not self._transport.is_closing():
            from fastapi.exceptions import HTTPException
            if isinstance(exc, HTTPException):
                detail = exc.detail if isinstance(exc.detail, (dict, list)) else {"detail": exc.detail}
                resp = self._core.build_response(detail, exc.status_code, keep_alive)
                if resp:
                    self._transport.write(resp)
                    return
            logger.exception("Unhandled exception in endpoint")
            self._transport.write(_500_RESP)

    # ── Async endpoint dispatch ──────────────────────────────────────────

    async def _handle_async(self, coro: Any, first_yield: Any, status_code: int, keep_alive: bool) -> None:
        """Await async endpoint coroutine, serialize + write response.

        C++ partially drove the coroutine via PyIter_Send and passed
        the yielded awaitable (first_yield). We finish execution here.
        Properly forwards exceptions back into the coroutine via throw()
        so user-defined exception handlers (e.g. try/except in endpoints) work.
        """
        try:
            raw = await _drive_coro(coro, first_yield)
            transport = self._transport
            if transport and not transport.is_closing():
                # FAST PATH — dict/list are the most common return types (90%+)
                raw_type = type(raw)
                if raw_type is dict or raw_type is list or raw_type is str or raw_type is int or raw_type is float or raw_type is bool or raw is None:
                    resp = self._core.build_response(raw, status_code, keep_alive)
                    if resp:
                        transport.write(resp)
                    else:
                        transport.write(_500_RESP)
                # StreamingResponse — chunked transfer encoding
                elif hasattr(raw, 'body_iterator'):
                    status = getattr(raw, 'status_code', 200)
                    headers_list = []
                    if hasattr(raw, 'raw_headers'):
                        headers_list = raw.raw_headers
                    elif hasattr(raw, 'headers'):
                        for k, v in raw.headers.items():
                            headers_list.append((k, v))
                    # Pre-built status line replaces 4× extend() with single lookup + extend
                    prefix = _STREAMING_STATUS_LINES.get(status)
                    if prefix is not None:
                        buf = bytearray(prefix)
                    else:
                        buf = bytearray(f"HTTP/1.1 {status} OK\r\ntransfer-encoding: chunked\r\n".encode())
                    for name, value in headers_list:
                        if isinstance(name, str):
                            name = name.encode("latin-1")
                        if isinstance(value, str):
                            value = value.encode("latin-1")
                        buf.extend(name)
                        buf.extend(b": ")
                        buf.extend(value)
                        buf.extend(b"\r\n")
                    buf.extend(b"\r\n")
                    transport.write(bytes(buf))
                    try:
                        async for chunk in raw.body_iterator:
                            if isinstance(chunk, str):
                                chunk = chunk.encode("utf-8")
                            if chunk:
                                transport.write(_build_chunked_frame(chunk))
                        transport.write(b"0\r\n\r\n")
                    except Exception:
                        logger.warning("Streaming response error", exc_info=True)
                        try:
                            transport.write(b"0\r\n\r\n")
                        except Exception:
                            logger.debug("Failed to write chunked trailer", exc_info=True)
                # Handle Response objects (HTMLResponse, etc.)
                elif hasattr(raw, "body") and hasattr(raw, "status_code"):
                    self._write_response_obj(raw, keep_alive)
                else:
                    # Pydantic models → dict for JSON serialization
                    if hasattr(raw, 'model_dump'):
                        raw = raw.model_dump(mode='json')
                    resp = self._core.build_response(raw, status_code, keep_alive)
                    if resp:
                        transport.write(resp)
                    else:
                        transport.write(_500_RESP)
            # BackgroundTasks support
            background = getattr(raw, 'background', None)
            if background is not None:
                await background()
        except Exception as exc:
            self._write_error(exc, keep_alive)
        finally:
            self._core.record_request_end()

    async def _handle_async_di(
        self, di_coro: Any, first_yield: Any, endpoint: Any, kwargs: dict,
        status_code: int, keep_alive: bool
    ) -> None:
        """Complete async DI resolution, then call endpoint + write response.

        C++ partially drove di_coro via PyIter_Send and passed
        the yielded awaitable (first_yield). We finish DI resolution here.
        """
        try:
            # Resume DI coroutine with the yielded awaitable
            solved = await _drive_coro(di_coro, first_yield)
            if isinstance(solved, tuple) and len(solved) >= 2:
                values, errors = solved[0], solved[1]
                if errors:
                    if self._transport and not self._transport.is_closing():
                        resp = self._core.build_response(
                            {"detail": list(errors)}, 422, keep_alive)
                        if resp:
                            self._transport.write(resp)
                    return
                if isinstance(values, dict):
                    kwargs.update(values)

            # Call endpoint
            raw = await endpoint(**kwargs)
            if self._transport and not self._transport.is_closing():
                # FAST PATH — dict/list are the most common return types
                raw_type = type(raw)
                if raw_type is dict or raw_type is list or raw_type is str or raw_type is int or raw_type is float or raw_type is bool or raw is None:
                    resp = self._core.build_response(raw, status_code, keep_alive)
                    if resp:
                        self._transport.write(resp)
                    else:
                        self._transport.write(_500_RESP)
                else:
                    # Pydantic models → dict for JSON serialization
                    if hasattr(raw, 'model_dump'):
                        raw = raw.model_dump(mode='json')
                    resp = self._core.build_response(raw, status_code, keep_alive)
                    if resp:
                        self._transport.write(resp)
                    else:
                        self._transport.write(_500_RESP)
            # BackgroundTasks support
            background = getattr(raw, 'background', None)
            if background is not None:
                await background()
        except Exception as exc:
            self._write_error(exc, keep_alive)
        finally:
            self._core.record_request_end()

    # ── Pydantic body validation path ────────────────────────────────────

    async def _handle_pydantic(self, ir: Any) -> None:
        """Handle routes needing Pydantic validation (POST with body models)."""
        try:
            if ir.has_body_params and ir.json_body is not None:
                body_values, body_errors = await _request_body_to_args(
                    body_fields=ir.body_params,
                    received_body=ir.json_body,
                    embed_body_fields=ir.embed_body_fields,
                )
                if body_errors:
                    if self._transport and not self._transport.is_closing():
                        resp = self._core.build_response(
                            {"detail": body_errors}, 422, True)
                        if resp:
                            self._transport.write(resp)
                    return
                ir.kwargs.update(body_values)

            raw = await ir.endpoint(**ir.kwargs)
            status_code = int(ir.status_code) if ir.status_code else 200
            if self._transport and not self._transport.is_closing():
                if hasattr(raw, 'model_dump'):
                    raw = raw.model_dump(mode='json')
                resp = self._core.build_response(raw, status_code, True)
                if resp:
                    self._transport.write(resp)
                else:
                    self._transport.write(_500_RESP)
            # BackgroundTasks support
            background = getattr(raw, 'background', None)
            if background is not None:
                await background()
        except Exception as exc:
            self._write_error(exc, True)
        finally:
            self._core.record_request_end()

    # ── WebSocket lifecycle handler ──────────────────────────────────────

    async def _handle_websocket(self, endpoint: Any, path_params: dict) -> None:
        """Run WebSocket endpoint with CppWebSocket wrapper."""
        ws = self._ws
        if not ws:
            return
        try:
            kwargs = dict(path_params) if path_params else {}
            # Inject WebSocket — use cached signature (pre-computed at registration)
            ep_id = id(endpoint)
            ws_param = _ws_sig_cache.get(ep_id, "websocket")
            kwargs[ws_param] = ws
            await endpoint(**kwargs)
        except Exception as exc:
            ws._metrics.errors += 1
            raise
        finally:
            try:
                if not ws._closed:
                    await ws.close(1000)
            except Exception:
                pass
            try:
                if self._transport and not self._transport.is_closing():
                    self._transport.close()
            except Exception:
                pass

    # ── Keep-alive (batch sweep — no per-connection timers) ─────────────

    def _ka_reset(self, now: float = 0.0) -> None:
        # Update deadline only — batch sweep handles expiry checking
        if now == 0.0:
            now = self._loop.time()
        self._ka_deadline = now + self._ka_timeout

    def _ka_cancel(self) -> None:
        # Mark as not subject to keep-alive timeout
        self._ka_deadline = 0.0


# ═════════════════════════════════════════════════════════════════════════════
# Server creation (reusable by TestClient)
# ═════════════════════════════════════════════════════════════════════════════


async def _create_server(
    app: Any, host: str = "127.0.0.1", port: int = 8000,
    keep_alive_timeout: float = 75.0,
) -> asyncio.AbstractServer:
    """Create and return a C++ HTTP server without blocking.

    Used by ``run_server()`` and ``TestClient``. The caller is responsible
    for ``server.close()`` / ``server.wait_closed()``.
    """
    loop = asyncio.get_event_loop()
    core_app = app._core_app

    # Sync routes if not yet done
    if hasattr(app, "_sync_routes_to_core"):
        app._sync_routes_to_core()

    # Sync CORS config to C++ core
    if hasattr(core_app, "configure_cors"):
        for mw in getattr(app, "user_middleware", []):
            cls = getattr(mw, "cls", None) or (mw[0] if isinstance(mw, (list, tuple)) else None)
            if cls and getattr(cls, "__name__", "") == "CORSMiddleware":
                kw = getattr(mw, "kwargs", {}) or (mw[2] if isinstance(mw, (list, tuple)) and len(mw) > 2 else {})
                try:
                    core_app.configure_cors(
                        allow_origins=kw.get("allow_origins", []),
                        allow_origin_regex=kw.get("allow_origin_regex"),
                        allow_methods=kw.get("allow_methods", ["GET"]),
                        allow_headers=kw.get("allow_headers", []),
                        allow_credentials=kw.get("allow_credentials", False),
                        expose_headers=kw.get("expose_headers", []),
                        max_age=kw.get("max_age", 600),
                    )
                except Exception:
                    pass
                break

    # Freeze routes
    if hasattr(core_app, "freeze_routes"):
        core_app.freeze_routes()

    # ── Warm-up: eliminate first-request lazy initialization overhead ──
    _init_cached_refs()       # Pre-import modules + intern strings in C++
    _prewarm_buffer_pool(4)   # Pre-allocate thread-local response buffers
    if hasattr(core_app, 'warmup'):
        core_app.warmup()     # Exercise parse→route→serialize→build to warm icache

    # Cache OpenAPI schema
    if hasattr(core_app, "set_openapi_schema") and hasattr(app, "openapi"):
        try:
            from fastapi._json_utils import json_dumps_str
            schema = app.openapi()
            if schema:
                schema_json = json_dumps_str(schema)
                core_app.set_openapi_schema(schema_json)
        except Exception:
            pass

    active_connections: set[CppHttpProtocol] = set()
    active_count = [0]  # mutable int via list — avoids nonlocal overhead

    # Pre-populate protocol pool to avoid __init__ on first connections
    _PREWARM = 512
    for _ in range(_PREWARM if _MAX_CONNECTIONS == 0 else min(_PREWARM, _MAX_CONNECTIONS)):
        proto = CppHttpProtocol(core_app, loop, keep_alive_timeout, None)
        _protocol_pool.release(proto)

    def _protocol_factory() -> asyncio.Protocol:
        if _MAX_CONNECTIONS > 0 and active_count[0] >= _MAX_CONNECTIONS:
            return _RejectProtocol()
        active_count[0] += 1
        if _PRESSURE_THRESHOLD > 0 and active_count[0] > _PRESSURE_THRESHOLD and not core_app.force_close:
            core_app.force_close = 1
        proto = _protocol_pool.acquire(
            core_app, loop, keep_alive_timeout, active_connections)
        proto._active_count = active_count
        active_connections.add(proto)
        return proto

    server = await loop.create_server(
        _protocol_factory, host, port,
        reuse_address=True, backlog=65535,
    )

    # TCP_FASTOPEN
    for s in server.sockets or []:
        try:
            s.setsockopt(socket.IPPROTO_TCP, 23, 5)
        except (OSError, AttributeError):
            pass

    return server


# ═════════════════════════════════════════════════════════════════════════════
# Server startup
# ═════════════════════════════════════════════════════════════════════════════

async def run_server(
    app: Any, host: str = "127.0.0.1", port: int = 8000,
    keep_alive_timeout: float = 75.0,
) -> None:
    """Start the C++ HTTP server with optimal event loop configuration."""
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target = min(65536, hard)
        if soft < target:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    except (ImportError, ValueError, OSError):
        pass

    # Increase TCP connection queue limits (Linux/WSL — silent on other platforms)
    try:
        import subprocess
        for _cmd in (
            "sysctl -w net.core.somaxconn=65535",
            "sysctl -w net.ipv4.tcp_max_syn_backlog=65535",
            "sysctl -w net.core.netdev_max_backlog=65535",
        ):
            subprocess.run(_cmd.split(), capture_output=True, timeout=2)
    except Exception:
        pass

    loop = asyncio.get_event_loop()

    # ── GC control: disable automatic collection to prevent multi-second gen2 pauses
    # Default thresholds (700/10/10) cause gen2 scans of ALL live objects.
    # With 10K connections that's millions of objects → multi-second stalls.
    # Rely on CPython refcounting for short-lived request objects.
    gc.disable()
    gc.collect(0)  # flush pending gen0 before disabling

    # Enable eager task execution (Python 3.12+)
    try:
        loop.set_task_factory(asyncio.eager_task_factory)  # type: ignore[attr-defined]
    except AttributeError:
        pass

    core_app = app._core_app

    # ── Sync CORS config to C++ core ─────────────────────────────────────
    if hasattr(core_app, "configure_cors"):
        for mw in getattr(app, "user_middleware", []):
            cls = getattr(mw, "cls", None) or (mw[0] if isinstance(mw, (list, tuple)) else None)
            if cls and getattr(cls, "__name__", "") == "CORSMiddleware":
                kw = getattr(mw, "kwargs", {}) or (mw[2] if isinstance(mw, (list, tuple)) and len(mw) > 2 else {})
                try:
                    core_app.configure_cors(
                        allow_origins=kw.get("allow_origins", []),
                        allow_origin_regex=kw.get("allow_origin_regex"),
                        allow_methods=kw.get("allow_methods", ["GET"]),
                        allow_headers=kw.get("allow_headers", []),
                        allow_credentials=kw.get("allow_credentials", False),
                        expose_headers=kw.get("expose_headers", []),
                        max_age=kw.get("max_age", 600),
                    )
                except Exception as exc:
                    print(f"[cpp-server] CORS sync failed: {exc}", file=sys.stderr)
                break

    # ── Configure C++ native middleware (silent) ────────────────────────
    for mw in getattr(app, "user_middleware", []):
        cls = getattr(mw, "cls", None) or (mw[0] if isinstance(mw, (list, tuple)) else None)
        cls_name = getattr(cls, "__name__", "") if cls else ""
        kw = getattr(mw, "kwargs", {}) or (mw[2] if isinstance(mw, (list, tuple)) and len(mw) > 2 else {})

        if cls_name == "RateLimitMiddleware":
            if hasattr(core_app, "configure_rate_limit"):
                max_req = kw.get("max_requests", 100)
                window = kw.get("window_seconds", 60)
                core_app.configure_rate_limit(True, max_req, window)

        elif cls_name == "LoggingMiddleware":
            if hasattr(core_app, "set_post_response_hook"):
                def _log_hook(method, path, status_code, duration_ms):
                    print(f"{method} {path} - {status_code} - {duration_ms/1000:.3f}s")
                core_app.set_post_response_hook(_log_hook)

    # Freeze routes — skip shared_lock in hot path after startup
    if hasattr(core_app, "freeze_routes"):
        core_app.freeze_routes()

    # ── Warm-up: eliminate first-request lazy initialization overhead ──
    _init_cached_refs()       # Pre-import modules + intern strings in C++
    _prewarm_buffer_pool(4)   # Pre-allocate thread-local response buffers
    if hasattr(core_app, 'warmup'):
        core_app.warmup()     # Exercise parse→route→serialize→build to warm icache

    # ── Lifespan context manager detection ──────────────────────────────
    router = getattr(app, "router", None)
    lifespan_handler = None
    if router is not None:
        lifespan_handler = getattr(router, "lifespan_context", None)

    # ── Lifespan context manager or on_startup/on_shutdown ────────────
    lifespan_cm = None
    if lifespan_handler is not None:
        # lifespan_handler is the user's async context manager factory
        lifespan_cm = lifespan_handler(app)
        state = await lifespan_cm.__aenter__()
        if state is not None:
            app_state = getattr(app, "state", None)
            if app_state is not None:
                state_dict = getattr(app_state, "_state", None)
                if state_dict is not None and isinstance(state_dict, dict):
                    state_dict.update(state)
                elif isinstance(state, dict):
                    # Fallback: set attributes directly on app.state
                    for k, v in state.items():
                        setattr(app_state, k, v)

    # Detect reload worker for fast shutdown
    _is_reload_worker = os.environ.get("_FASTAPI_RELOAD_WORKER") == "1"

    # ── Track active connections for graceful shutdown ──────────────────
    active_connections: set[CppHttpProtocol] = set()
    active_count = [0]  # mutable int via list — faster than len(set) in hot path

    # Pre-populate protocol pool BEFORE gc.freeze() so protocols are frozen
    _PREWARM = 512
    for _ in range(_PREWARM if _MAX_CONNECTIONS == 0 else min(_PREWARM, _MAX_CONNECTIONS)):
        proto = CppHttpProtocol(core_app, loop, keep_alive_timeout, None)
        _protocol_pool.release(proto)

    # ── gc.freeze(): move ALL startup objects to permanent generation ──
    # After warm-up + pool pre-warm, hundreds of thousands of objects exist
    # (protocols, modules, cached strings, route tables). gc.freeze() moves
    # them to a permanent generation that gc.collect(0/1/2) never scans.
    # Result: gen0 only tracks objects created by live request processing.
    # For sync endpoints, gen0 is nearly empty → scan < 0.1ms.
    gc.collect(0)
    gc.collect(1)
    gc.collect(2)   # full collection — clean up any startup cycles
    gc.freeze()     # move ALL survivors to permanent generation

    # ── GC strategy: freeze + refcounting, no runtime collections ───────
    # gc.freeze() above moved startup objects to permanent generation.
    # CPython refcounting handles short-lived request objects (coroutines,
    # tuples, dicts). NO periodic gc.collect() — it blocks the event loop
    # scanning live async task objects (measured: 60-70ms at 88K req/s).
    # Only collect during idle periods to prevent long-term cycle leaks.
    def _gc_maintenance():
        if active_count[0] == 0:
            gc.collect(1)
        loop.call_later(600.0, _gc_maintenance)

    loop.call_later(600.0, _gc_maintenance)

    def _protocol_factory() -> asyncio.Protocol:
        # Optional connection limit (disabled by default — no cap like uWS/Fiber/Bun)
        if _MAX_CONNECTIONS > 0 and active_count[0] >= _MAX_CONNECTIONS:
            return _RejectProtocol()
        active_count[0] += 1
        # Under pressure: force Connection: close to cycle connections and free slots
        if _PRESSURE_THRESHOLD > 0 and active_count[0] > _PRESSURE_THRESHOLD and not core_app.force_close:
            core_app.force_close = 1
        # OPT-14: Reuse protocol objects from pool to reduce __init__ + GC overhead
        proto = _protocol_pool.acquire(
            core_app, loop, keep_alive_timeout, active_connections)
        proto._active_count = active_count
        active_connections.add(proto)
        return proto

    try:
        server = await loop.create_server(
            _protocol_factory, host, port,
            reuse_address=True, reuse_port=True, backlog=65535,
        )
    except OSError:
        # reuse_port not supported on this platform — fall back
        server = await loop.create_server(
            _protocol_factory, host, port,
            reuse_address=True, backlog=65535,
        )

    # TCP_FASTOPEN: allow data in SYN packet, reducing handshake latency
    for s in server.sockets or []:
        try:
            s.setsockopt(socket.IPPROTO_TCP, 23, 5)  # TCP_FASTOPEN = 23
        except (OSError, AttributeError):
            pass

    print(f"C++ HTTP server running on http://{host}:{port}")
    print("Press Ctrl+C to stop")

    # ── Defer OpenAPI schema generation to background (non-blocking) ──────
    # OpenAPI is only needed for /docs and /openapi.json — no need to block
    # startup for it.  The openapi() method caches its result, so if a user
    # hits /docs before this task finishes, it generates on-demand once.
    if hasattr(core_app, "set_openapi_schema") and hasattr(app, "openapi"):
        async def _deferred_openapi() -> None:
            try:
                from fastapi._json_utils import json_dumps_str as _json_dumps_str
                schema = app.openapi()
                if schema:
                    schema_json = _json_dumps_str(schema)
                    core_app.set_openapi_schema(schema_json)
            except Exception:
                logger.debug("OpenAPI schema generation failed", exc_info=True)

        loop.create_task(_deferred_openapi())

    stop_event = asyncio.Event()

    def _signal_handler() -> None:
        print("\nShutting down...")
        # Force-close all active connections immediately to speed up drain
        for proto in list(active_connections):
            t = proto._transport
            if t and not t.is_closing():
                t.close()
        stop_event.set()

    if sys.platform != "win32":
        loop.add_signal_handler(signal.SIGINT, _signal_handler)
        loop.add_signal_handler(signal.SIGTERM, _signal_handler)
    else:
        # Windows: loop.add_signal_handler() not supported — use thread-safe signal
        signal.signal(signal.SIGINT, lambda *_: loop.call_soon_threadsafe(_signal_handler))
        signal.signal(signal.SIGTERM, lambda *_: loop.call_soon_threadsafe(_signal_handler))

    # Batch keep-alive sweep — one task checks all connections every 15s
    # instead of per-connection call_later() timers (eliminates ~1K+ callbacks/sec)
    async def _ka_sweep() -> None:
        while not stop_event.is_set():
            await asyncio.sleep(15.0)
            now = time.monotonic()
            batch: list = []
            for p in active_connections:
                if p._ka_deadline > 0 and now > p._ka_deadline:
                    batch.append(p)
                if len(batch) >= 100:
                    for proto in batch:
                        t = proto._transport
                        if t and not t.is_closing():
                            t.close()
                    batch.clear()
                    await asyncio.sleep(0)  # yield to event loop
            for proto in batch:
                t = proto._transport
                if t and not t.is_closing():
                    t.close()

    sweep_task = loop.create_task(_ka_sweep())

    # ── Idle pool trim: reclaim memory when connections drop ─────────────
    async def _pool_trim() -> None:
        while not stop_event.is_set():
            await asyncio.sleep(60.0)
            target = max(256, active_count[0] // 2)
            _protocol_pool.trim(target)

    trim_task = loop.create_task(_pool_trim())

    try:
        async with server:
            await stop_event.wait()
    except KeyboardInterrupt:
        pass
    finally:
        gc.unfreeze()  # move frozen objects back for cleanup
        gc.enable()    # re-enable GC for clean interpreter shutdown
        sweep_task.cancel()
        trim_task.cancel()
        # ── Graceful shutdown: stop accepting, drain active connections ─
        server.close()
        await server.wait_closed()

        # Short drain for reload workers; normal drain otherwise
        drain_timeout = 0.5 if _is_reload_worker else 5.0

        # Wait for active connections to drain (with timeout)
        if active_connections:
            logger.info(
                "Waiting for %d active connection(s) to drain...",
                len(active_connections),
            )
            drain_start = time.monotonic()
            while active_connections and (time.monotonic() - drain_start) < drain_timeout:
                await asyncio.sleep(0.1)
            # Force-close any remaining connections after timeout
            if active_connections:
                logger.warning(
                    "Force-closing %d connection(s) after drain timeout",
                    len(active_connections),
                )
                for proto in list(active_connections):
                    transport = proto._transport
                    if transport and not transport.is_closing():
                        transport.close()
                active_connections.clear()

        # ── Lifespan: shutdown ─────────────────────────────────────────
        if lifespan_cm is not None:
            await lifespan_cm.__aexit__(None, None, None)

        print("Server stopped.")
