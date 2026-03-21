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
from time import monotonic as _monotonic
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
    202: "Accepted",
    203: "Non-Authoritative Information",
    204: "No Content",
    205: "Reset Content",
    206: "Partial Content",
    207: "Multi-Status",
    208: "Already Reported",
    301: "Moved Permanently",
    302: "Found",
    303: "See Other",
    304: "Not Modified",
    307: "Temporary Redirect",
    308: "Permanent Redirect",
    400: "Bad Request",
    401: "Unauthorized",
    403: "Forbidden",
    404: "Not Found",
    405: "Method Not Allowed",
    406: "Not Acceptable",
    408: "Request Timeout",
    409: "Conflict",
    410: "Gone",
    413: "Payload Too Large",
    415: "Unsupported Media Type",
    422: "Unprocessable Entity",
    429: "Too Many Requests",
    500: "Internal Server Error",
    501: "Not Implemented",
    502: "Bad Gateway",
    503: "Service Unavailable",
    504: "Gateway Timeout",
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
            # Clear stale done waiter — happens when C++ resolved it last tick
            # but this coroutine hasn't run yet to create a new one.
            w = self._waiter
            if w is not None and w.done():
                self._waiter = None
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
        http_buf_get_view as _http_buf_get_view,
        http_buf_consume as _http_buf_consume,
        http_buf_clear as _http_buf_clear,
        http_buf_len as _http_buf_len,
        http_buf_append as _http_buf_append,
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
    from fastapi._core_bridge import (
        InlineResult as _InlineResult,
        encode_to_json_bytes as _encode_to_json_bytes,
    )
except ImportError:
    _InlineResult = None
    _encode_to_json_bytes = None

# ── App-level exception handler registry (set once at server start) ──────────
_app_exc_handlers: dict = {}
_app_status_handlers: dict = {}


def _set_app_exception_handlers(exc_handlers: dict, status_handlers: dict) -> None:
    """Register app exception handlers once at server start for async dispatch."""
    global _app_exc_handlers, _app_status_handlers
    _app_exc_handlers = exc_handlers
    _app_status_handlers = status_handlers


# HTTP middleware dispatch functions registered at startup.
# Each entry is an async callable(dispatch_func, request) -> response.
# Empty list = zero overhead on hot path (no middleware registered).
_http_middleware_dispatchers: list = []


def _set_http_middleware_dispatchers(dispatchers: list) -> None:
    global _http_middleware_dispatchers
    _http_middleware_dispatchers = dispatchers


# Maps raw WS endpoint function id -> (route.app, route) for DI-wrapped dispatch.
_ws_app_map: dict = {}  # id(endpoint) -> (app_callable, route)
_full_app: Any = None  # full ASGI app (with middleware) for WS dispatch

# ContextVar: current route for scope["route"] injection in C++ fast path
from contextvars import ContextVar as _ContextVar2
_current_route: _ContextVar2 = _ContextVar2("_current_route", default=None)


def _build_ws_app_map(app: Any) -> None:
    """Build endpoint->(app, route) map for all WebSocket routes."""
    global _ws_app_map, _full_app
    _ws_app_map = {}
    # Build middleware-wrapped app for WebSocket dispatch
    _full_app = app
    user_mw = getattr(app, "user_middleware", [])
    if user_mw:
        # Build middleware stack: innermost = router, outermost = first middleware
        ws_stack = app.router
        for mw in reversed(user_mw):
            # Unwrap nested Middleware descriptors to get the actual callable
            _mw = mw
            while hasattr(_mw, "cls") and hasattr(getattr(_mw, "cls", None), "cls"):
                _mw = _mw.cls
            cls = getattr(_mw, "cls", None)
            args = getattr(_mw, "args", ())
            kw = getattr(_mw, "kwargs", {})
            if cls is not None and callable(cls):
                try:
                    ws_stack = cls(ws_stack, *args, **kw)
                except Exception:
                    pass
        _full_app = ws_stack
    try:
        from fastapi.routing import APIWebSocketRoute
        from fastapi._routing_base import WebSocketRoute as _WebSocketRoute
        routes = getattr(getattr(app, 'router', None), 'routes', []) or []
        for route in routes:
            if isinstance(route, (APIWebSocketRoute, _WebSocketRoute)):
                _ws_app_map[id(route.endpoint)] = (route.app, route)
    except Exception:
        pass


# Test-mode exception capture (raise_server_exceptions support)
_raise_server_exceptions: bool = False
_last_server_exception = None

# ContextVar: stores the raw query string for the current request.
# Set in data_received() before C++ processes the request so that
# dep_solver can inject params for dependency_overrides that introduce
# new query parameters not in the original FieldSpec.
from contextvars import ContextVar as _ContextVar
_current_query_string: _ContextVar[bytes] = _ContextVar('_current_query_string', default=b'')
_current_raw_headers: _ContextVar = _ContextVar('_current_raw_headers', default=None)
_current_method: _ContextVar = _ContextVar('_current_method', default='GET')
_current_path: _ContextVar = _ContextVar('_current_path', default='/')


def _set_raise_server_exceptions(enabled: bool) -> None:
    global _raise_server_exceptions
    _raise_server_exceptions = enabled


def _pop_server_exception():
    global _last_server_exception
    exc = _last_server_exception
    _last_server_exception = None
    return exc


def _set_last_server_exception(exc: Exception) -> None:
    global _last_server_exception
    _last_server_exception = exc
    return exc


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
        "__weakref__",  # required for WsConnectionGroup WeakSet membership
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
        # Code 1006 = abnormal closure - encode as close frame with reason prefix
        # so test clients can recover the original code and reason
        if code == 1006:
            encoded_reason = f"1006:{reason}" if reason else "1006:"
            code = 1000
            reason = encoded_reason
        if self._corked:
            self._flush_cork()
        # Flush any pending auto-corked writes
        if self._pending_sends:
            self._flush_pending()
        if reason:
            _payload = struct.pack("!H", code) + reason.encode("utf-8")
            frame = self._build_frame_py(0x8, _payload)
        else:
            frame = _ws_build_close_frame_bytes(code) if _ws_build_close_frame_bytes else self._build_frame_py(0x8, struct.pack("!H", code))
        try:
            self._transport.write(frame)
        except Exception:
            logger.debug("WS close frame write failed", exc_info=True)
            return
        # Wait for peer close response (graceful handshake, max 2s)
        try:
            close_wait = self._loop.create_future()
            self._close_waiter = close_wait
            await asyncio.wait_for(close_wait, timeout=2.0)
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
            if self._last_received is not None and (data is self._last_received or data == self._last_received):
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
        # ASGI WebSocket protocol: first receive must return websocket.connect
        if self.application_state == WebSocketState.CONNECTING:
            return {"type": "websocket.connect"}
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
            await self.close(code=message.get("code", 1000), reason=message.get("reason"))

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

    def __init__(self, max_size: int = 32768):
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
_PRESSURE_THRESHOLD = _MAX_CONNECTIONS * 4 // 5 if _MAX_CONNECTIONS > 0 else 16384
_WORKER_MODE = os.environ.get("_FASTAPI_WORKER") == "1"
_RATE_LIMIT_ENABLED = False  # Set True after middleware config if RateLimitMiddleware present


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

def _inject_headers_into_response(resp: bytes, extra_headers: list) -> bytes:
    """Inject extra headers into pre-built HTTP response bytes, before \r\n\r\n."""
    sep = b'\r\n\r\n'
    idx = resp.find(sep)
    if idx == -1:
        return resp
    extra = b''
    for k, v in extra_headers:
        if isinstance(k, str): k = k.encode('latin-1')
        if isinstance(v, str): v = v.encode('latin-1')
        if k.lower() == b'content-length': continue
        extra += b'\r\n' + k + b': ' + v
    return resp[:idx] + extra + resp[idx:] if extra else resp


async def _drive_coro(coro: Any, first_yield: Any) -> Any:
    """Drive a partially-consumed coroutine to completion.

    C++ drove the first step via PyIter_Send; this finishes the rest.
    Mimics asyncio Task.__step: on await exception, throws it back into
    the coroutine so user-defined handlers (try/except) work correctly.

    Fast-path: most async endpoints have exactly 1 await (e.g. a DB query).
    We inline the single-yield case to avoid the loop + extra try/except.
    """
    # Single-yield fast-path (most common case)
    # first_yield may be None (from asyncio.sleep(0) in Python 3.12+ which uses __sleep0)
    if first_yield is None:
        result = None
    else:
        result = await first_yield
    try:
        next_yield = coro.send(result)
    except StopIteration as e:
        return e.value
    # Multi-yield: fall through to loop
    while True:
        if next_yield is None:
            result = None
        else:
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
                continue
        try:
            next_yield = coro.send(result)
        except StopIteration as e:
            return e.value


async def _run_http_middleware(
    raw: Any,
    dispatchers: list,
    extra_headers: list,
    status_code: int,
    keep_alive: bool,
    transport: Any,
    core: Any,
    sock_fd: int,
    raw_headers: Any,
    method: str,
    path: str,
) -> Any:
    """Run @app.middleware("http") chain after endpoint execution.

    Builds a minimal Request and a pre-populated Response, runs each
    middleware dispatch function, then writes the final response.
    Only called when _http_middleware_dispatchers is non-empty.
    """
    from fastapi.requests import Request
    from fastapi.responses import Response, JSONResponse

    # Build minimal ASGI scope for Request
    header_list = []
    if raw_headers:
        for item in raw_headers:
            if isinstance(item, (list, tuple)) and len(item) == 2:
                name, value = item
                if isinstance(name, str): name = name.lower().encode('latin-1')
                elif isinstance(name, bytes): name = name.lower()
                if isinstance(value, str): value = value.encode('latin-1')
                header_list.append((name, value))

    # Parse path and query string
    _path, _, _qs = path.partition('?')
    scope = {
        "type": "http",
        "method": method if isinstance(method, str) else method.decode('latin-1'),
        "path": _path or '/',
        "query_string": _qs.encode('latin-1') if _qs else b'',
        "headers": header_list,
        "root_path": "",
    }

    # Build pre-populated response from endpoint result
    _raw_type = type(raw)
    if _raw_type is dict or _raw_type is list:
        resp_obj = JSONResponse(content=raw, status_code=status_code)
    elif hasattr(raw, 'body') or hasattr(raw, 'media_type'):
        resp_obj = raw  # already a Response
    else:
        resp_obj = JSONResponse(content=raw, status_code=status_code)
    # Preserve shim-attached lifecycle objects (bg tasks, exit_stack)
    _cpp_bg = getattr(raw, '__cpp_bg_tasks__', None)
    _cpp_stack = getattr(raw, '__cpp_exit_stack__', None)
    if _cpp_bg is not None: resp_obj.__cpp_bg_tasks__ = _cpp_bg
    if _cpp_stack is not None: resp_obj.__cpp_exit_stack__ = _cpp_stack

    # Apply extra headers from sub-deps
    if extra_headers:
        for k, v in extra_headers:
            _k = k.decode('latin-1') if isinstance(k, bytes) else k
            _v = v.decode('latin-1') if isinstance(v, bytes) else v
            resp_obj.headers[_k] = _v

    # Build call_next that returns the pre-built response
    async def call_next(request: Any) -> Any:
        return resp_obj

    # Run middleware chain in reverse order (last registered = outermost)
    request = Request(scope)
    current_response = resp_obj
    for dispatch_fn in reversed(dispatchers):
        _prev = current_response
        async def _call_next_for(req: Any, _r=_prev) -> Any:
            return _r
        current_response = await dispatch_fn(request, _call_next_for)
        if current_response is None:
            current_response = _prev

    # Write final response
    final = current_response
    # Propagate lifecycle objects to final response if not already present
    if not hasattr(final, '__cpp_bg_tasks__') and hasattr(resp_obj, '__cpp_bg_tasks__'):
        final.__cpp_bg_tasks__ = resp_obj.__cpp_bg_tasks__
    if not hasattr(final, '__cpp_exit_stack__') and hasattr(resp_obj, '__cpp_exit_stack__'):
        final.__cpp_exit_stack__ = resp_obj.__cpp_exit_stack__
    if transport and not transport.is_closing():
        _sc = getattr(final, 'status_code', status_code)
        _body = getattr(final, 'body', None)
        if _body is not None and isinstance(_body, bytes):
            # Response object: build from parts
            # _build_response_from_parts expects list of (str, str) tuples
            _hdrs = []
            for _k, _v in final.headers.items():
                _hdrs.append((
                    _k.decode('latin-1') if isinstance(_k, bytes) else _k,
                    _v.decode('latin-1') if isinstance(_v, bytes) else _v,
                ))
            transport.write(_build_response_from_parts(_sc, _hdrs, _body, keep_alive))
        else:
            resp = core.build_response_from_any(final, _sc, keep_alive)
            if resp is not None:
                transport.write(resp)
    return final


async def _write_chunked_streaming(transport: Any, raw: Any) -> None:
    """Write a StreamingResponse via chunked transfer encoding.

    Single implementation shared by all async handlers — eliminates
    code duplication and reduces instruction cache pressure.
    Supports both async and sync body iterators.
    """
    status = getattr(raw, 'status_code', 200)
    headers_list = getattr(raw, '_raw_headers', None) or getattr(raw, 'raw_headers', [])
    if not headers_list and hasattr(raw, 'headers'):
        headers_list = list(raw.headers.items())
    prefix = _STREAMING_STATUS_LINES.get(status)
    if prefix is not None:
        buf = bytearray(prefix)
    else:
        phrase = _STATUS_PHRASES.get(status, "")
        buf = bytearray(f"HTTP/1.1 {status} {phrase}\r\ntransfer-encoding: chunked\r\n".encode())
    hdr_parts: list = []
    for hname, hvalue in headers_list:
        if isinstance(hname, str):
            hname = hname.encode("latin-1")
        if isinstance(hvalue, str):
            hvalue = hvalue.encode("latin-1")
        hdr_parts.append(hname)
        hdr_parts.append(b": ")
        hdr_parts.append(hvalue)
        hdr_parts.append(b"\r\n")
    hdr_parts.append(b"\r\n")
    buf.extend(b"".join(hdr_parts))
    transport.write(bytes(buf))
    body_iter = raw.body_iterator
    try:
        if hasattr(body_iter, '__aiter__'):
            async for chunk in body_iter:
                if isinstance(chunk, str):
                    chunk = chunk.encode("utf-8")
                if chunk:
                    transport.write(_build_chunked_frame(chunk))
        else:
            for chunk in body_iter:
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

    __slots__ = ("_core", "_transport", "_http_buf", "_ka_deadline", "_ka_timeout", "_loop", "_wr_paused", "_ws", "_ws_handler", "_ws_ring_buf", "_ws_fd", "_ws_ping_handle", "_ws_pong_received", "_ws_task", "_connections_set", "_pending_tasks", "_pending_tasks_discard", "_active_count", "_sock_fd", "_core_batch", "_core_append_dispatch",
             "_loop_create_task",  # cached bound method — saves LOAD_ATTR per async create_task
             "_ka_needs_reset")    # flag: True = received data, sweep updates _ka_deadline lazily

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
        self._pending_tasks_discard = None  # Cached set.discard — avoids bound-method alloc per async req
        self._sock_fd = -1  # Raw socket FD for direct C++ HTTP writes (TCP_QUICKACK re-armed in C++)
        # Cache handle_http_batch_v2 bound method — avoids PyMethodObject alloc per data_received call
        _v2 = getattr(core_app, 'handle_http_batch_v2', None)
        self._core_batch = _v2 if _v2 is not None else core_app.handle_http_batch
        # Fused append+dispatch: eliminates one Python→C++ call per data_received
        self._core_append_dispatch = getattr(core_app, 'handle_http_append_and_dispatch', None)
        # Cache create_task — avoids dict lookup on loop object per async endpoint dispatch
        self._loop_create_task = loop.create_task
        # Flag: set True on data_received; sweep updates _ka_deadline lazily (no _monotonic() per req)
        self._ka_needs_reset = False

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
        self._ka_needs_reset = False
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
        _v2 = getattr(core_app, 'handle_http_batch_v2', None)
        self._core_batch = _v2 if _v2 is not None else core_app.handle_http_batch
        self._core_append_dispatch = getattr(core_app, 'handle_http_append_and_dispatch', None)
        self._loop_create_task = loop.create_task
        self._ka_needs_reset = False

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
        # Write buffer limits: 32KB high / 8KB low per connection.
        # 100 conns × 32KB = 3.2MB vs previous 64/128KB = 6.4/12.8MB.
        # asyncio only applies back-pressure after high-water — 32KB is
        # still large enough for any single response body.
        transport.set_write_buffer_limits(high=32768, low=8192)  # type: ignore[union-attr]
        # SO_BUSY_POLL: spin-poll for 100μs before sleeping — reduces scheduler wake-up jitter
        # at high connection counts. No-op on non-Linux / WSL without the option.
        try:
            sock.setsockopt(socket.SOL_SOCKET, 46, 100)  # SO_BUSY_POLL = 46 (Linux)
        except (OSError, AttributeError):
            pass
        if _RATE_LIMIT_ENABLED:
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
            # Resolve server-initiated close waiter if TCP drops mid-handshake
            cw = self._ws._close_waiter
            if cw is not None and not cw.done():
                cw.set_result(None)
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
        # TCP_QUICKACK re-armed inside C++ handle_http_append_and_dispatch() / handle_http_batch_v2().

        # WebSocket frame handling (after upgrade) — dispatch to mode-specific handler
        if self._ws is not None:
            self._ws_pong_received = True
            self._ws_handler(data)
            return

        http_buf = self._http_buf

        # Back-pressure: buffer data only, don't dispatch until writes resume
        if self._wr_paused:
            if not _http_buf_append(http_buf, data):
                transport = self._transport
                if transport and not transport.is_closing():
                    transport.write(b"HTTP/1.1 413 Payload Too Large\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                    transport.close()
            return

        transport = self._transport
        if transport is None:
            return

        sock_fd = self._sock_fd

        # Extract query string from raw HTTP request line and store in ContextVar.
        # This allows dep_solver to inject params for dependency_overrides that
        # introduce new query parameters not in the original FieldSpec.
        _qs = b''
        _nl = data.find(b'\r\n')
        if _nl > 0:
            _req_line = data[:_nl]
            _q = _req_line.find(b'?')
            if _q >= 0:
                _sp = _req_line.find(b' ', _q)
                _qs = _req_line[_q + 1:_sp] if _sp > _q else _req_line[_q + 1:]
        _current_query_string.set(_qs)
        # Parse raw headers and method/path for dep Request injection
        try:
            _hdrs_end = data.find(b'\r\n\r\n')
            if _hdrs_end > 0:
                _hdr_block = data[:_hdrs_end]
                _hdr_lines = _hdr_block.split(b'\r\n')
                _req_parts = _hdr_lines[0].split(b' ') if _hdr_lines else []
                _method = _req_parts[0].decode('latin-1') if len(_req_parts) > 0 else 'GET'
                _full_path = _req_parts[1].decode('latin-1') if len(_req_parts) > 1 else '/'
                _path_only = _full_path.split('?')[0]
                _parsed_hdrs = []
                for _hl in _hdr_lines[1:]:
                    _colon = _hl.find(b':')
                    if _colon > 0:
                        _parsed_hdrs.append((_hl[:_colon].strip().lower(), _hl[_colon+1:].strip()))
                _current_raw_headers.set(_parsed_hdrs)
                _current_method.set(_method)
                _current_path.set(_path_only)
        except Exception:
            pass

        # ── Get first result: fused append+dispatch if available, else separate ────────────
        core_ad = self._core_append_dispatch
        if core_ad is not None:
            # Single C++ call: appends data + dispatches
            # Returns: 0=done+empty, 1=done+partial, -1=need-more, -2=error, -3=overflow, tuple=async
            result = core_ad(http_buf, data, transport, sock_fd)
            if type(result) is int:
                if result >= 0:
                    if result == 0:
                        self._ka_needs_reset = True  # lazy: sweep updates actual deadline
                    return  # sync batch done (0=empty, 1=partial pending)
                if result == -1:
                    return  # need more data
                if result == -2:
                    transport.close()
                    _http_buf_clear(http_buf)
                    return
                # -3: buffer overflow
                if not transport.is_closing():
                    transport.write(b"HTTP/1.1 413 Payload Too Large\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                    transport.close()
                return
            # tuple: PYGEN_NEXT / async / WebSocket — fall through to dispatch loop
        else:
            # Fallback: separate append then dispatch (older .so)
            if not _http_buf_append(http_buf, data):
                if not transport.is_closing():
                    transport.write(b"HTTP/1.1 413 Payload Too Large\r\ncontent-length: 0\r\nconnection: close\r\n\r\n")
                    transport.close()
                return
            core_batch = self._core_batch
            result = core_batch(http_buf, transport, sock_fd)
            if type(result) is int:
                if result >= 0:
                    if result == 0:
                        self._ka_needs_reset = True  # lazy: sweep updates actual deadline
                    return
                if result == -2:
                    transport.close()
                    _http_buf_clear(http_buf)
                return  # -1 or other negative: need more or error
            if result is None:
                return
            if result is False:
                transport.close()
                _http_buf_clear(http_buf)
                return

        # ── Dispatch loop — handles async/ws/stream/async_di/InlineResult ────────────────
        # Reached only when result is a tuple or InlineResult (not int/None/False).
        # IR and core_batch loaded here — not on the sync hot path.
        IR = _InlineResult
        core_batch = self._core_batch
        create_task = self._loop_create_task
        while True:
            r_type = type(result)

            if result is True:
                # No WS route found - 101 already sent, close gracefully
                if not transport.is_closing():
                    close_frame = _ws_build_close_frame_bytes(1000) if _ws_build_close_frame_bytes else CppWebSocket._build_frame_py(0x8, bytes([0x03, 0xe8]))
                    transport.write(close_frame)
                    transport.close()
                _http_buf_clear(http_buf)
                return

            if r_type is tuple:
                tag = result[0]
                if tag == "ws":
                    rlen = len(result)
                    ws_path = result[3] if rlen >= 4 else "/"
                    ws_headers = result[4] if rlen >= 5 else []
                    ws_query_string = result[5] if rlen >= 6 else b""
                    endpoint, path_params = result[1], result[2]
                    if endpoint is None:
                        if not transport.is_closing():
                            # Send WS close frame (1000) before TCP close
                            close_frame = _ws_build_close_frame_bytes(1000) if _ws_build_close_frame_bytes else CppWebSocket._build_frame_py(0x8, bytes([0x03, 0xe8]))
                            transport.write(close_frame)
                            transport.close()
                        _http_buf_clear(http_buf)
                        return
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
                    self._ws_task = create_task(
                        self._handle_websocket(endpoint, path_params))
                    # Any remaining bytes are WebSocket frames that arrived with the upgrade
                    remaining_mv = _http_buf_get_view(http_buf)
                    remaining = bytes(remaining_mv) if remaining_mv is not None else b""
                    if remaining_mv is not None:
                        remaining_mv.release()
                    _http_buf_clear(http_buf)
                    if remaining:
                        self._ws_handler(remaining)
                    return  # connection is now WebSocket

                elif tag == "async":
                    _, coro, first_yield, status_code, keep_alive = result
                    if first_yield is not None:
                        try:
                            first_yield._asyncio_future_blocking = False
                        except AttributeError:
                            pass
                    task = create_task(
                        self._handle_async(coro, first_yield, status_code, keep_alive))
                    if not task.done():
                        pt = self._pending_tasks
                        if pt is None:
                            pt = self._pending_tasks = set()
                            self._pending_tasks_discard = pt.discard
                        pt.add(task)
                        task.add_done_callback(self._pending_tasks_discard)

                elif tag == "stream":
                    _, raw_resp, status_code, keep_alive = result
                    task = create_task(
                        self._handle_stream(raw_resp, status_code, keep_alive))
                    if not task.done():
                        pt = self._pending_tasks
                        if pt is None:
                            pt = self._pending_tasks = set()
                            self._pending_tasks_discard = pt.discard
                        pt.add(task)
                        task.add_done_callback(self._pending_tasks_discard)

                elif tag == "async_di":
                    _, di_coro, first_yield, endpoint, kwargs, sc, ka = result
                    if first_yield is not None:
                        try:
                            first_yield._asyncio_future_blocking = False
                        except AttributeError:
                            pass
                    task = create_task(
                        self._handle_async_di(di_coro, first_yield, endpoint, kwargs, sc, ka))
                    if not task.done():
                        pt = self._pending_tasks
                        if pt is None:
                            pt = self._pending_tasks = set()
                            self._pending_tasks_discard = pt.discard
                        pt.add(task)
                        task.add_done_callback(self._pending_tasks_discard)

            elif IR and r_type is IR:
                task = create_task(self._handle_pydantic(result))
                if not task.done():
                    pt = self._pending_tasks
                    if pt is None:
                        pt = self._pending_tasks = set()
                        self._pending_tasks_discard = pt.discard
                    pt.add(task)
                    task.add_done_callback(self._pending_tasks_discard)

            # Fetch next pipelined request (most often returns int = no more data)
            result = core_batch(http_buf, transport, sock_fd)
            if type(result) is int:
                if result == 0:
                    self._ka_needs_reset = True  # lazy ka deadline update
                elif result == -2:
                    transport.close()
                    _http_buf_clear(http_buf)
                # -1 (need more), 1 (partial), or other negative: just return
                return
            if result is None:
                return
            if result is False:
                transport.close()
                _http_buf_clear(http_buf)
                return
            # else: another tuple/InlineResult — continue dispatching

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
                ws = self._ws
                if ws and not ws._closed:
                    ws._closed = True
                    ws.client_state = WebSocketState.DISCONNECTED
                    # Resolve server-initiated close waiter (client ACK'd our close frame)
                    cw = ws._close_waiter
                    if cw is not None and not cw.done():
                        cw.set_result(None)
                    if frames_fed == 0:
                        # Protocol error path: C++ did NOT feed close to channel.
                        # Wake up any pending receive so the endpoint gets
                        # WebSocketDisconnect(code=1002) instead of hanging forever.
                        ws.feed_frame(0x8, close_payload)
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
                ws._closed = True
                ws.client_state = WebSocketState.DISCONNECTED
                cw = ws._close_waiter
                if cw is not None and not cw.done():
                    cw.set_result(None)
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
                ws._closed = True
                ws.client_state = WebSocketState.DISCONNECTED
                cw = ws._close_waiter
                if cw is not None and not cw.done():
                    cw.set_result(None)
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
        # Process buffered data that arrived during backpressure.
        # Data is already in the C++ buffer — just trigger processing with 0 new bytes.
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

    async def _dispatch_exception(self, exc: Exception, keep_alive: bool) -> None:
        """Dispatch exception to registered app handler or built-in error response (OPT-A)."""
        transport = self._transport
        if not transport or transport.is_closing():
            return
        from fastapi.exceptions import HTTPException, RequestValidationError
        from fastapi._concurrency import is_async_callable, run_in_threadpool
        # Walk MRO to find a registered app-level exception handler
        handler = None
        for cls in type(exc).__mro__:
            if cls in _app_exc_handlers:
                handler = _app_exc_handlers[cls]
                break
        if handler is None and isinstance(exc, HTTPException):
            handler = _app_status_handlers.get(exc.status_code)
        if handler is not None:
            try:
                if is_async_callable(handler):
                    resp = await handler(None, exc)
                else:
                    resp = await run_in_threadpool(handler, None, exc)
                if resp is not None and transport and not transport.is_closing():
                    resp_bytes = self._core.build_response_from_any(
                        resp, getattr(resp, 'status_code', 500), keep_alive)
                    if resp_bytes is not None:
                        transport.write(resp_bytes)
            except Exception:
                pass
            return
        # Built-in fallbacks
        if isinstance(exc, HTTPException):
            detail = exc.detail if isinstance(exc.detail, (dict, list)) else {"detail": exc.detail}
            exc_headers = getattr(exc, 'headers', None)
            if exc_headers:
                from fastapi._json_utils import json_dumps
                body = json_dumps(detail)
                hdrs: list = [(b"content-type", b"application/json")]
                for k, v in exc_headers.items():
                    hdrs.append((
                        k.encode("latin-1") if isinstance(k, str) else k,
                        v.encode("latin-1") if isinstance(v, str) else v,
                    ))
                transport.write(
                    _build_response_from_parts(exc.status_code, hdrs, body, keep_alive)
                )
            else:
                resp = self._core.build_response(detail, exc.status_code, keep_alive)
                if resp:
                    transport.write(resp)
            return
        if isinstance(exc, RequestValidationError):
            try:
                from fastapi._core_bridge import serialize_error_response
                body = serialize_error_response(exc.errors())
                transport.write(
                    _build_response_from_parts(422, [(b"content-type", b"application/json")], body, keep_alive)
                )
                return
            except Exception:
                pass
        logger.exception("Unhandled exception in endpoint")
        if _raise_server_exceptions:
            global _last_server_exception
            _last_server_exception = exc
        transport.write(_500_RESP)

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
                # OPT-D: plain dict/list fast path — skips full type dispatch
                _raw_type = type(raw)
                if _raw_type is dict or _raw_type is list:
                    # OPT-write_async_result: zero-alloc direct fd write (no PyBytes)
                    self._core.write_async_result(raw, transport, status_code, keep_alive, self._sock_fd)
                    # dicts/lists never have BackgroundTasks — skip getattr
                else:
                    # OPT-3.4: Unified C++ type dispatch (str/int/bool/None/Pydantic/Response)
                    # Returns None only for StreamingResponse (needs Python async iteration).
                    # Run HTTP middleware if registered
                    if _http_middleware_dispatchers:
                        raw = await _run_http_middleware(
                            raw, _http_middleware_dispatchers,
                            [], status_code, keep_alive,
                            transport, self._core, self._sock_fd,
                            None, 'GET', '/'
                        )
                    else:
                        # BackgroundTasks: run BEFORE writing response so TestClient sees them
                        background = getattr(raw, 'background', None)
                        if background is not None:
                            await background()
                        _shim_bg = getattr(raw, '__cpp_bg_tasks__', None)
                        if _shim_bg is not None and getattr(_shim_bg, 'tasks', None):
                            await _shim_bg()
                        resp = self._core.build_response_from_any(raw, status_code, keep_alive)
                        if resp is not None:
                            transport.write(resp)
                        else:
                            await _write_chunked_streaming(transport, raw)
                    _shim_stack = getattr(raw, '__cpp_exit_stack__', None)
                    if _shim_stack is not None:
                        await _shim_stack.aclose()
        except Exception as exc:
            await self._dispatch_exception(exc, keep_alive)
        finally:
            self._core.record_request_end()

    async def _handle_stream(self, raw: Any, status_code: int, keep_alive: bool) -> None:
        """Handle StreamingResponse/FileResponse returned directly from a fast-path endpoint.

        Called when the C++ fast path gets PYGEN_RETURN with an object that needs
        async streaming (e.g. AsyncGenerator body_iterator) or file reading (FileResponse).
        """
        try:
            transport = self._transport
            if transport and not transport.is_closing():
                raw_path = getattr(raw, 'path', None)
                if raw_path is not None and not hasattr(raw, 'body_iterator'):
                    # FileResponse — read file bytes synchronously
                    # (Blocking I/O is acceptable; tests use small files)
                    raw_path_str = str(raw_path)
                    try:
                        with open(raw_path_str, 'rb') as _fh:
                            file_bytes = _fh.read()
                    except OSError:
                        file_bytes = b""
                    st = getattr(raw, 'status_code', 200)
                    headers_list = getattr(raw, '_raw_headers', None) or getattr(raw, 'raw_headers', [])
                    has_cl = any(
                        (n if isinstance(n, bytes) else n.encode('latin-1')).lower() == b'content-length'
                        for n, _ in headers_list
                    )
                    hdr_parts = [f"HTTP/1.1 {st} {_STATUS_PHRASES.get(st, 'OK')}".encode()]
                    for hname, hvalue in headers_list:
                        if isinstance(hname, str):
                            hname = hname.encode("latin-1")
                        if isinstance(hvalue, str):
                            hvalue = hvalue.encode("latin-1")
                        hdr_parts.append(b"\r\n" + hname + b": " + hvalue)
                    if not has_cl:
                        hdr_parts.append(b"\r\ncontent-length: " + str(len(file_bytes)).encode())
                    conn = b"\r\nconnection: keep-alive\r\n\r\n" if keep_alive else b"\r\nconnection: close\r\n\r\n"
                    hdr_parts.append(conn)
                    http_bytes = b"".join(hdr_parts) + file_bytes
                    transport.write(http_bytes)
                else:
                    # StreamingResponse — chunked transfer encoding
                    await _write_chunked_streaming(transport, raw)
            background = getattr(raw, 'background', None)
            if background is not None:
                await background()
        except Exception as exc:
            await self._dispatch_exception(exc, keep_alive)
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
            # Set current route for scope["route"] injection
            _ep_id = id(endpoint)
            _route_token = None
            try:
                from fastapi.routing import _endpoint_id_to_route as _eitr
                _rt = _eitr.get(_ep_id)
                if _rt is not None:
                    _route_token = _current_route.set(_rt)
            except Exception:
                pass
            # Resume DI coroutine with the yielded awaitable
            solved = await _drive_coro(di_coro, first_yield)
            # SolvedDependency namedtuple: (values, errors, background_tasks, response, dep_cache)
            di_bg_tasks = None
            _di_extra_headers: list = []
            if isinstance(solved, tuple) and len(solved) >= 2:
                values, errors = solved[0], solved[1]
                # Extract background_tasks (index 2) accumulated during DI resolution.
                # Sub-dependencies that receive BackgroundTasks and add tasks are captured here.
                if len(solved) >= 3:
                    di_bg_tasks = solved[2]
                # OPT-B: Extract sub_response (index 3) — headers set by sub-deps that
                # declare `response: Response` and append headers to it.
                if len(solved) >= 4 and solved[3] is not None:
                    _sub = solved[3]
                    _raw_hdrs = getattr(_sub, 'raw_headers', None)
                    if _raw_hdrs:
                        _di_extra_headers = list(_raw_hdrs)
                    elif hasattr(_sub, 'headers'):
                        for _k, _v in _sub.headers.items():
                            _di_extra_headers.append((
                                _k.encode("latin-1") if isinstance(_k, str) else _k,
                                _v.encode("latin-1") if isinstance(_v, str) else _v,
                            ))
                    _di_sc = getattr(_sub, 'status_code', None)
                    if _di_sc is not None and _di_sc != 200:
                        status_code = _di_sc
                if errors:
                    if self._transport and not self._transport.is_closing():
                        resp = self._core.build_response(
                            {"detail": list(errors)}, 422, keep_alive)
                        if resp:
                            self._transport.write(resp)
                    return
                if isinstance(values, dict):
                    # Extract generator dep exit_stack before updating kwargs
                    exit_stack = values.pop('__exit_stack__', None)
                    kwargs.update(values)
            else:
                exit_stack = None

            # Inject BackgroundTasks into endpoint kwargs if endpoint declares it
            try:
                from fastapi.routing import _endpoint_id_to_route as _eitr2
                _ep_route = _eitr2.get(id(endpoint))
                _ep_bg_param = getattr(getattr(_ep_route, 'dependant', None), 'background_tasks_param_name', None)
                if _ep_bg_param and _ep_bg_param not in kwargs:
                    from fastapi.background import BackgroundTasks as _BT
                    if di_bg_tasks is None:
                        di_bg_tasks = _BT()
                    kwargs[_ep_bg_param] = di_bg_tasks
            except Exception:
                pass
            # Call endpoint — handle sync vs async, and wrap with exit_stack for gen dep cleanup
            _is_async_endpoint = inspect.iscoroutinefunction(endpoint) or inspect.iscoroutinefunction(inspect.unwrap(endpoint))

            # Capture response object from DI values for post-endpoint header merging
            _di_response_obj = solved[3] if isinstance(solved, tuple) and len(solved) >= 4 else None

            async def _call_and_write() -> Any:
                """Call endpoint and write response (used both in and out of exit_stack)."""
                if _is_async_endpoint:
                    _raw = await endpoint(**kwargs)
                else:
                    _raw = endpoint(**kwargs)
                # Merge response headers set by endpoint (response: Response param)
                nonlocal _di_extra_headers
                if _di_response_obj is not None:
                    _post_hdrs = []
                    _raw_hdrs2 = getattr(_di_response_obj, "raw_headers", None)
                    if _raw_hdrs2:
                        _post_hdrs = list(_raw_hdrs2)
                    elif hasattr(_di_response_obj, "headers"):
                        for _k2, _v2 in _di_response_obj.headers.items():
                            _post_hdrs.append((
                                _k2.encode("latin-1") if isinstance(_k2, str) else _k2,
                                _v2.encode("latin-1") if isinstance(_v2, str) else _v2,
                            ))
                    if _post_hdrs:
                        # Skip content-length/content-type to avoid overwriting body headers
                        _di_extra_headers = [
                            (k, v) for k, v in _post_hdrs
                            if (k if isinstance(k, bytes) else k.encode()).lower()
                            not in (b'content-length', b'content-type')
                        ]
                    _di_sc2 = getattr(_di_response_obj, "status_code", None)
                    if _di_sc2 is not None and _di_sc2 != 200:
                        status_code = _di_sc2
                transport = self._transport
                if transport and not transport.is_closing():
                    # Run HTTP middleware if registered (@app.middleware("http"))
                    # Zero overhead when empty: branch not taken.
                    if _http_middleware_dispatchers:
                        _raw = await _run_http_middleware(
                            _raw, _http_middleware_dispatchers,
                            _di_extra_headers, status_code, keep_alive,
                            transport, self._core, self._sock_fd,
                            kwargs.get('__raw_headers__'), kwargs.get('__method__', 'GET'),
                            kwargs.get('__path__', '/')
                        )
                    else:
                        _raw_type = type(_raw)
                        if _raw_type is dict or _raw_type is list:
                            if _di_extra_headers and _encode_to_json_bytes is not None:
                                # OPT-B: merge sub-dep response headers into JSON response
                                _body = _encode_to_json_bytes(_raw)
                                _merged = [("content-type", "application/json")] + [
                                    (k.decode("latin-1") if isinstance(k, bytes) else k,
                                     v.decode("latin-1") if isinstance(v, bytes) else v)
                                    for k, v in _di_extra_headers
                                ]
                                transport.write(_build_response_from_parts(status_code, _merged, _body, keep_alive))
                            else:
                                # OPT-write_async_result: zero-alloc direct fd write
                                self._core.write_async_result(_raw, transport, status_code, keep_alive, self._sock_fd)
                        else:
                            resp = self._core.build_response_from_any(_raw, status_code, keep_alive)
                            if resp is not None:
                                if _di_extra_headers:
                                    # Inject sub-dep headers into pre-built response bytes
                                    resp = _inject_headers_into_response(resp, _di_extra_headers)
                                transport.write(resp)
                            else:
                                # StreamingResponse — chunked transfer encoding
                                await _write_chunked_streaming(transport, _raw)
                    # Run shim bg tasks (from BackgroundTasks param) after middleware
                    _shim_bg = getattr(_raw, '__cpp_bg_tasks__', None)
                    if _shim_bg is not None and getattr(_shim_bg, 'tasks', None):
                        await _shim_bg()
                return _raw

            if exit_stack is not None:
                # Write response AND run background tasks INSIDE exit_stack so
                # cleanup (generator dep teardown) happens AFTER bg tasks complete.
                # This matches FastAPI semantics: bg tasks see pre-cleanup dep state.
                async with exit_stack:
                    raw = await _call_and_write()
                    background = getattr(raw, 'background', None)
                    if background is not None:
                        await background()
                    if di_bg_tasks is not None and getattr(di_bg_tasks, 'tasks', None):
                        await di_bg_tasks()
            else:
                raw = await _call_and_write()
                background = getattr(raw, 'background', None)
                if background is not None:
                    await background()
                if di_bg_tasks is not None and getattr(di_bg_tasks, 'tasks', None):
                    await di_bg_tasks()
        except Exception as exc:
            await self._dispatch_exception(exc, keep_alive)
        finally:
            if _route_token is not None:
                _current_route.reset(_route_token)
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
            transport = self._transport
            if transport and not transport.is_closing():
                _raw_type = type(raw)
                if _raw_type is dict or _raw_type is list:
                    _resp = self._core.build_response(raw, status_code, True)
                    if _resp:
                        transport.write(_resp)
                    # dicts/lists never carry BackgroundTasks
                else:
                    resp = self._core.build_response_from_any(raw, status_code, True)
                    if resp is not None:
                        transport.write(resp)
                    else:
                        await _write_chunked_streaming(transport, raw)
                    # BackgroundTasks support
                    background = getattr(raw, 'background', None)
                    if background is not None:
                        await background()
        except Exception as exc:
            await self._dispatch_exception(exc, True)
        finally:
            self._core.record_request_end()

    # ── WebSocket lifecycle handler ──────────────────────────────────────

    async def _handle_websocket(self, endpoint: Any, path_params: dict) -> None:
        """Run WebSocket endpoint with CppWebSocket wrapper."""
        ws = self._ws
        if not ws:
            return
        try:
            ep_id = id(endpoint)
            ws_entry = _ws_app_map.get(ep_id)
            if ws_entry is not None:
                ws_app, ws_route = ws_entry
                ws.scope["route"] = ws_route
                # Inject app exception handlers so wrap_app_handling_exceptions works
                if "starlette.exception_handlers" not in ws.scope:
                    _exc_h = _app_exc_handlers
                    _status_h = {k: v for k, v in _exc_h.items() if isinstance(k, int)}
                    _cls_h = {k: v for k, v in _exc_h.items() if not isinstance(k, int)}
                    ws.scope["starlette.exception_handlers"] = (_cls_h, _status_h)
                # Use middleware-wrapped stack if available, else direct ws_app
                _dispatch = _full_app if (_full_app is not None and _full_app is not ws_route.app) else ws_app
                await _dispatch(ws.scope, ws.receive, ws.send)
            else:
                kwargs = dict(path_params) if path_params else {}
                ws_param = _ws_sig_cache.get(ep_id, "websocket")
                kwargs[ws_param] = ws
                await endpoint(**kwargs)
        except WebSocketDisconnect:
            # Normal lifecycle — client closed the connection. Not an error.
            pass
        except Exception as exc:
            ws._metrics.errors += 1
            # Log but do NOT re-raise: _ws_task is never awaited by anyone, so
            # re-raising produces asyncio's "Task exception was never retrieved".
            logger.error("Unhandled exception in WebSocket endpoint", exc_info=True)
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
            now = _monotonic()
        self._ka_deadline = now + self._ka_timeout

    def _ka_cancel(self) -> None:
        # Mark as not subject to keep-alive timeout
        self._ka_deadline = 0.0


# ═════════════════════════════════════════════════════════════════════════════
# Server creation (reusable by TestClient)
# ═════════════════════════════════════════════════════════════════════════════


async def _create_server(
    app: Any, host: str = "127.0.0.1", port: int = 8000,
    keep_alive_timeout: float = 30.0,
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

    # OPT-A: Register app exception handlers for async dispatch paths
    _set_app_exception_handlers(
        dict(getattr(app, 'exception_handlers', {})),
        {k: v for k, v in getattr(app, 'exception_handlers', {}).items() if isinstance(k, int)},
    )

    # Build WS app map for DI-wrapped WebSocket dispatch
    _build_ws_app_map(app)

    # Register @app.middleware("http") dispatch functions for async response path
    _http_dispatchers = []
    for _mw in getattr(app, 'user_middleware', []):
        _cls = getattr(_mw, 'cls', None) or (_mw[0] if isinstance(_mw, (list, tuple)) else None)
        _cls_name = getattr(_cls, '__name__', '')
        if _cls_name == 'BaseHTTPMiddleware':
            _kw = getattr(_mw, 'kwargs', {}) or {}
            _dispatch = _kw.get('dispatch')
            if _dispatch is not None:
                _http_dispatchers.append(_dispatch)
    _set_http_middleware_dispatchers(_http_dispatchers)

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
    if app.openapi_url and hasattr(core_app, "set_openapi_schema") and hasattr(app, "openapi"):
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
    _PREWARM = int(os.environ.get("FASTAPI_PREWARM_CONNS", "512"))
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

    # TCP_FASTOPEN + TCP_DEFER_ACCEPT (Linux)
    for s in server.sockets or []:
        try:
            s.setsockopt(socket.IPPROTO_TCP, 23, 5)  # TCP_FASTOPEN
        except (OSError, AttributeError):
            pass
        try:
            s.setsockopt(socket.IPPROTO_TCP, 9, 1)  # TCP_DEFER_ACCEPT
        except (OSError, AttributeError):
            pass

    return server


# ═════════════════════════════════════════════════════════════════════════════
# Master-accept fd receiver (multi-worker connection dispatch)
# ═════════════════════════════════════════════════════════════════════════════


def _setup_fd_receiver(
    loop: asyncio.AbstractEventLoop,
    unix_sock: socket.socket,
    protocol_factory: Any,
) -> None:
    """Receive accepted connection fds from master via SCM_RIGHTS.

    Master process does all accept() and round-robins fds to workers over
    Unix domain socketpairs.  This callback fires when the master sends a
    fd, wraps it in a native asyncio transport via connect_accepted_socket(),
    and request processing begins at full uvloop speed.

    Pattern: Node.js cluster SCHED_RR (default on Linux).
    """
    import struct as _struct

    unix_sock.setblocking(False)
    _cmsg_size = socket.CMSG_LEN(_struct.calcsize("i"))

    def _on_readable() -> None:
        while True:
            try:
                msg, ancdata, _, _ = unix_sock.recvmsg(1, _cmsg_size)
                if not msg:
                    loop.remove_reader(unix_sock.fileno())
                    return
                for level, type_, data in ancdata:
                    if level == socket.SOL_SOCKET and type_ == socket.SCM_RIGHTS:
                        fd = _struct.unpack("i", data[: _struct.calcsize("i")])[0]
                        conn_sock = socket.fromfd(
                            fd, socket.AF_INET, socket.SOCK_STREAM
                        )
                        os.close(fd)
                        conn_sock.setblocking(False)
                        loop.create_task(
                            loop.connect_accepted_socket(
                                protocol_factory, conn_sock
                            )
                        )
            except BlockingIOError:
                return
            except OSError:
                loop.remove_reader(unix_sock.fileno())
                return

    loop.add_reader(unix_sock.fileno(), _on_readable)


def _setup_fd_receiver_win(
    loop: asyncio.AbstractEventLoop,
    dispatch_sock: socket.socket,
    protocol_factory: Any,
) -> None:
    """Receive accepted connections from master via socket.share() (Windows).

    Uses a dedicated reader thread because ProactorEventLoop doesn't support
    add_reader(). Reads length-prefixed share_data from the dispatch socket,
    reconstructs the connection with fromshare(), and schedules
    connect_accepted_socket on the event loop.
    """
    import threading as _threading_win

    def _recv_exact(sock: socket.socket, n: int) -> bytes | None:
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def _reader() -> None:
        while True:
            try:
                hdr = _recv_exact(dispatch_sock, 4)
                if not hdr:
                    break
                length = int.from_bytes(hdr, "little")
                share_data = _recv_exact(dispatch_sock, length)
                if not share_data:
                    break
                conn_sock = socket.fromshare(share_data)
                conn_sock.setblocking(False)
                asyncio.run_coroutine_threadsafe(
                    loop.connect_accepted_socket(protocol_factory, conn_sock),
                    loop,
                )
            except OSError:
                break

    t = _threading_win.Thread(target=_reader, daemon=True)
    t.start()


# ═════════════════════════════════════════════════════════════════════════════
# Server startup
# ═════════════════════════════════════════════════════════════════════════════

async def run_server(
    app: Any, host: str = "127.0.0.1", port: int = 8000,
    keep_alive_timeout: float = 30.0,
    sock: Any = None,
    unix_sock: Any = None,
) -> None:
    """Start the C++ HTTP server with optimal event loop configuration."""
    from fastapi._multiworker import is_worker as _is_worker_check
    _is_worker = _is_worker_check()

    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target = min(65536, hard)
        if soft < target:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    except (ImportError, ValueError, OSError):
        pass

    # Sysctl tuning — skip in worker mode (parent already ran it).
    # Write directly to /proc/sys/ — no subprocess fork+exec overhead.
    if not _is_worker:
        _sysctl_map = {
            "/proc/sys/net/core/somaxconn": "65535",
            "/proc/sys/net/ipv4/tcp_max_syn_backlog": "65535",
            "/proc/sys/net/core/netdev_max_backlog": "65535",
            "/proc/sys/net/ipv4/tcp_tw_reuse": "1",
            "/proc/sys/net/ipv4/tcp_fin_timeout": "10",
        }
        for _path, _val in _sysctl_map.items():
            try:
                with open(_path, "w") as _f:
                    _f.write(_val)
            except OSError:
                pass  # not root / not Linux — silently ignore

    loop = asyncio.get_event_loop()

    # ── Scheduler tuning: reduce OS preemption jitter (Linux/WSL2 only) ──────────
    # Root cause of benchmark variance: Hyper-V vCPU steal time + Linux CFS jitter.
    # SCHED_FIFO prevents CFS from preempting this process for any normal-priority
    # task, and reduces Hyper-V VMBus interrupt frequency (fewer context switches
    # → fewer hypervisor interventions). Requires root / CAP_SYS_NICE.
    if sys.platform != "win32":
        _cpu_aff = os.environ.get("FASTAPI_CPU_AFFINITY", "")
        if _cpu_aff:
            try:
                _cpuset = {int(x.strip()) for x in _cpu_aff.split(",") if x.strip()}
                os.sched_setaffinity(0, _cpuset)
            except (AttributeError, ValueError, OSError, PermissionError):
                pass
        _rt_prio = int(os.environ.get("FASTAPI_RT_PRIORITY", "0"))
        if _rt_prio > 0:
            try:
                import ctypes as _ctypes
                _libc = _ctypes.CDLL("libc.so.6", use_errno=True)
                class _SchedParam(_ctypes.Structure):
                    _fields_ = [("sched_priority", _ctypes.c_int)]
                # SCHED_FIFO=1: real-time FIFO, no time-slicing, not preemptable by CFS
                _libc.sched_setscheduler(0, 1, _ctypes.byref(_SchedParam(_rt_prio)))
            except Exception:
                pass  # non-root, non-Linux, or WSL2 without privilege — silently skip

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
                global _RATE_LIMIT_ENABLED
                _RATE_LIMIT_ENABLED = True

        elif cls_name == "LoggingMiddleware":
            if hasattr(core_app, "set_post_response_hook"):
                def _log_hook(method, path, status_code, duration_ms):
                    print(f"{method} {path} - {status_code} - {duration_ms/1000:.3f}s")
                core_app.set_post_response_hook(_log_hook)

    # Freeze routes — skip shared_lock in hot path after startup
    # Workers skip: parent already froze before fork (avoids COW page fault)
    if not _is_worker and hasattr(core_app, "freeze_routes"):
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
    # Protocol pool prewarm: enough to absorb the first burst without allocation.
    # Keep it modest — each protocol holds a 16 KB C++ http_buf.
    # 64 covers typical burst; OS will handle the rest via pool reuse.
    _PREWARM = int(os.environ.get("FASTAPI_PREWARM_CONNS", "64" if _is_worker else "128"))
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
    # Skip gen2 — scanning all pydantic/starlette objects at startup costs 50-100ms.
    # CPython refcounting handles short-lived objects; gen2 cycles are rare at startup.
    gc.freeze()     # move ALL survivors to permanent generation

    # ── GC strategy: freeze + refcounting, no runtime collections ───────
    # gc.freeze() above moved startup objects to permanent generation.
    # CPython refcounting handles short-lived request objects (coroutines,
    # tuples, dicts). NO periodic gc.collect() — it blocks the event loop
    # scanning live async task objects (measured: 60-70ms at 88K req/s).
    # Only collect during idle periods to prevent long-term cycle leaks.
    # Safety net: if RSS grows >100MB since last check under sustained load,
    # do a fast gen0 collection to catch cyclic refs from user code.
    _last_rss = [0]
    try:
        import resource as _resource
        _last_rss[0] = _resource.getrusage(_resource.RUSAGE_SELF).ru_maxrss
    except (ImportError, AttributeError):
        pass  # Windows — fall back to no RSS tracking

    def _gc_maintenance():
        if active_count[0] == 0:
            gc.collect(1)
        else:
            # Safety net: prevent unbounded memory growth from cyclic user code.
            # RSS threshold lowered to 50 MB (was 100 MB) for faster detection.
            try:
                import resource as _resource
                current_rss = _resource.getrusage(_resource.RUSAGE_SELF).ru_maxrss
                if _last_rss[0] > 0 and current_rss > _last_rss[0] + 50_000:
                    gc.collect(0)  # fast gen0 only (<1ms)
                _last_rss[0] = current_rss
            except (ImportError, AttributeError):
                pass
        loop.call_later(30.0, _gc_maintenance)  # 30s instead of 600s — catches cycles sooner

    loop.call_later(30.0, _gc_maintenance)

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

    if unix_sock is not None and _is_worker:
        # Master-accept mode: master does all accept() and round-robins
        # connections to workers. Platform-specific dispatch mechanism.
        if sys.platform == "win32":
            _setup_fd_receiver_win(loop, unix_sock, _protocol_factory)
        else:
            _setup_fd_receiver(loop, unix_sock, _protocol_factory)
        server = None
    elif sock is not None:
        # Shared socket (Windows subprocess via socket.fromshare)
        server = await loop.create_server(
            _protocol_factory, sock=sock, backlog=65535,
        )
    else:
        # Single-process mode — create own socket
        server = await loop.create_server(
            _protocol_factory, host, port,
            reuse_address=True, backlog=65535,
        )

    # TCP_FASTOPEN / TCP_DEFER_ACCEPT on server sockets (not for accept thread)
    if server is not None:
        for s in server.sockets or []:
            try:
                s.setsockopt(socket.IPPROTO_TCP, 23, 5)  # TCP_FASTOPEN = 23
            except (OSError, AttributeError):
                pass
            try:
                s.setsockopt(socket.IPPROTO_TCP, 9, 1)  # TCP_DEFER_ACCEPT = 9
            except (OSError, AttributeError):
                pass

    # Print only in single-process mode — parent already printed in multi-worker
    if not _is_worker:
        print(f"C++ HTTP server running on http://{host}:{port}")
        print("Press Ctrl+C to stop")

    # ── Defer OpenAPI schema generation to background (non-blocking) ──────
    # OpenAPI is only needed for /docs and /openapi.json — no need to block
    # startup for it.  The openapi() method caches its result, so if a user
    # hits /docs before this task finishes, it generates on-demand once.
    # Workers skip: each would redundantly compute the same schema.
    # Only run for single-process or worker 0.
    _worker_id = os.environ.get("_FASTAPI_WORKER_ID", "0")
    if _worker_id == "0" and app.openapi_url and hasattr(core_app, "set_openapi_schema") and hasattr(app, "openapi"):
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

    # Batch keep-alive sweep — one task checks all connections every 5s
    # instead of per-connection call_later() timers (eliminates ~1K+ callbacks/sec)
    async def _ka_sweep() -> None:
        while not stop_event.is_set():
            await asyncio.sleep(5.0)
            now = _monotonic()
            batch: list = []
            for p in active_connections:
                # Lazy ka_deadline update: connection was active since last sweep
                if p._ka_needs_reset:
                    p._ka_needs_reset = False
                    p._ka_deadline = now + p._ka_timeout
                elif p._ka_deadline > 0 and now > p._ka_deadline:
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
        if server is not None:
            async with server:
                await stop_event.wait()
        else:
            # Accept thread mode — just wait for stop signal
            await stop_event.wait()
    except KeyboardInterrupt:
        pass
    finally:
        gc.unfreeze()  # move frozen objects back for cleanup
        gc.enable()    # re-enable GC for clean interpreter shutdown
        sweep_task.cancel()
        trim_task.cancel()
        # ── Graceful shutdown: stop accepting, drain active connections ─
        if server is not None:
            server.close()
            await server.wait_closed()
        elif unix_sock is not None:
            try:
                if sys.platform != "win32":
                    loop.remove_reader(unix_sock.fileno())
                unix_sock.close()
            except OSError:
                pass

        # Reset middleware dispatchers so they don't leak to subsequent servers
        _set_http_middleware_dispatchers([])

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
