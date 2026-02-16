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
import signal
import socket
import struct
import sys
import time
from collections import deque
from typing import Any

from starlette.websockets import WebSocketDisconnect, WebSocketState

# ── Zero-overhead awaitable for sync send methods ────────────────────────────
class _NoopAwaitable:
    """Awaitable that completes immediately with no overhead.
    Used by send_text/send_bytes to avoid coroutine frame creation."""
    __slots__ = ()
    def __await__(self):
        return iter(())

_NOOP = _NoopAwaitable()

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
                 '_high_water', '_low_water', '_paused')

    def __init__(self, loop: asyncio.AbstractEventLoop, protocol=None,
                 high_water: int = 256, low_water: int = 64):
        self._waiter: asyncio.Future | None = None
        self._buffer: deque = deque()  # O(1) popleft vs O(N) list.pop(0)
        self._loop = loop
        self._protocol = protocol  # back-ref for backpressure
        self._high_water = high_water
        self._low_water = low_water
        self._paused = False

    def feed(self, opcode: int, payload: bytes) -> None:
        """Feed a frame — called from data_received (sync context)."""
        waiter = self._waiter
        if waiter is not None and not waiter.done():
            self._waiter = None
            waiter.set_result((opcode, payload))
        else:
            self._buffer.append((opcode, payload))
            # Apply backpressure when buffer exceeds high water mark
            if (not self._paused
                    and len(self._buffer) >= self._high_water
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
            # Resume reading when buffer drops below low water mark
            if (self._paused
                    and len(buf) <= self._low_water
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
        ws_handle_echo_direct as _ws_handle_echo_direct,
        ws_handle_json_direct as _ws_handle_json_direct,
        # Frame building (send_text/send_bytes/send_json/close/ping)
        ws_build_frame_bytes as _ws_build_frame_bytes,
        ws_build_ping_frame as _ws_build_ping_frame,
        ws_build_close_frame_bytes as _ws_build_close_frame_bytes,
        ws_build_frames_batch as _ws_build_frames_batch,
        # JSON parse/serialize (receive_json/send_json)
        ws_parse_json as _ws_parse_json,
        ws_serialize_json as _ws_serialize_json,
        # Ring buffer lifecycle
        ws_ring_buffer_create as _ws_ring_buffer_create,
        ws_ring_buffer_reset as _ws_ring_buffer_reset,
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

# ── WebSocket endpoint signature cache ────────────────────────────────────────
_ws_sig_cache: dict[int, str] = {}

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
    """Feed parsed frames to WebSocket channel. Returns True if close detected."""
    channel = ws._channel
    waiter = channel._waiter
    buf = channel._buffer
    metrics = ws._metrics
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
        # Data frame — feed to channel directly
        metrics.messages_received += 1
        plen = len(payload) if isinstance(payload, (bytes, str)) else 0
        metrics.bytes_received += plen
        metrics.last_activity = time.monotonic()
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
        "_pending_writes", "_flush_scheduled",
        "_close_waiter", "_metrics",
    )

    def __init__(
        self,
        transport: asyncio.Transport,
        loop: asyncio.AbstractEventLoop,
        path: str = "/",
        path_params: dict | None = None,
    ) -> None:
        self._transport = transport
        self._loop = loop
        self._channel = _WsFastChannel(loop)  # protocol set later via _channel._protocol
        self._closed = False
        self._close_code = 1000
        self._corked = False
        self._cork_buf: list[tuple[int, bytes]] = []
        # Auto-cork: coalesce writes within same event loop tick
        self._pending_writes: list[bytes] = []
        self._flush_scheduled = False
        # Echo auto-detection
        self._protocol = None  # back-ref to CppHttpProtocol, set after creation
        self._echo_detect_count = 0
        self._last_received = None  # track last received for echo detection
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
            "headers": [],
            "query_string": b"",
        }

    # ── Lifecycle ─────────────────────────────────────────────────────

    async def accept(self, subprotocol: str | None = None, headers: list | None = None) -> None:
        """Accept the WebSocket connection (upgrade already sent by C++)."""
        self.application_state = WebSocketState.CONNECTED

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
        if self._pending_writes:
            self._flush_pending()
        frame = _ws_build_close_frame_bytes(code) if _ws_build_close_frame_bytes else self._build_frame_py(0x8, struct.pack("!H", code))
        try:
            self._transport.write(frame)
        except Exception:
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

    def _write_frame(self, frame: bytes) -> None:
        """Write frame with auto-cork: coalesces writes within same event loop tick."""
        transport = self._transport
        if transport is None or transport.is_closing():
            return
        pending = self._pending_writes
        pending.append(frame)
        if not self._flush_scheduled:
            self._flush_scheduled = True
            self._loop.call_soon(self._flush_pending)

    def _flush_pending(self) -> None:
        """Flush all pending writes as a single writev syscall."""
        self._flush_scheduled = False
        pending = self._pending_writes
        if not pending:
            return
        transport = self._transport
        if transport is None or transport.is_closing():
            pending.clear()
            return
        if len(pending) == 1:
            transport.write(pending[0])
        else:
            transport.writelines(pending)  # single writev syscall
        pending.clear()

    # ── Send methods ──────────────────────────────────────────────────

    def send_text(self, data: str) -> _NoopAwaitable:
        """Send a text message. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        # Echo auto-detection: if sending exactly what was received, switch handler
        # Require 3 consecutive echo matches to avoid false positives
        if _ws_handle_echo_direct is not None and self._echo_detect_count >= 0:
            if self._last_received is not None and data == self._last_received:
                self._echo_detect_count += 1
                if self._echo_detect_count >= 3:
                    proto = self._protocol
                    if proto is not None:
                        proto._ws_handler = proto._handle_ws_frames_echo
                    self._echo_detect_count = -1  # stop detecting
            else:
                self._echo_detect_count = 0  # non-match resets counter
            self._last_received = None
        payload = data if isinstance(data, bytes) else data.encode("utf-8")
        m = self._metrics
        m.messages_sent += 1
        m.bytes_sent += len(payload)
        m.last_activity = time.monotonic()
        if self._corked:
            self._cork_buf.append((0x1, payload))
            return _NOOP
        frame = _ws_build_frame_bytes(0x1, payload) if _ws_build_frame_bytes else self._build_frame_py(0x1, payload)
        self._write_frame(frame)
        return _NOOP

    def send_bytes(self, data: bytes) -> _NoopAwaitable:
        """Send a binary message. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        m = self._metrics
        m.messages_sent += 1
        m.bytes_sent += len(data)
        m.last_activity = time.monotonic()
        if self._corked:
            self._cork_buf.append((0x2, data))
            return _NOOP
        frame = _ws_build_frame_bytes(0x2, data) if _ws_build_frame_bytes else self._build_frame_py(0x2, data)
        self._write_frame(frame)
        return _NOOP

    def send_json(self, data: Any, mode: str = "text") -> _NoopAwaitable:
        """Send JSON data. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        if _ws_serialize_json is not None:
            json_bytes = _ws_serialize_json(data)
            opcode = 0x1 if mode == "text" else 0x2
            if self._corked:
                self._cork_buf.append((opcode, json_bytes))
                return _NOOP
            frame = _ws_build_frame_bytes(opcode, json_bytes) if _ws_build_frame_bytes else self._build_frame_py(opcode, json_bytes)
            self._write_frame(frame)
        else:
            import json
            payload = json.dumps(data).encode("utf-8")
            opcode = 0x1 if mode == "text" else 0x2
            frame = self._build_frame_py(opcode, payload)
            self._write_frame(frame)
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
            return payload
        if isinstance(payload, str):
            if _ws_parse_json is not None:
                return _ws_parse_json(payload)
            import json
            return json.loads(payload)
        # bytes
        if _ws_parse_json is not None:
            return _ws_parse_json(payload)
        import json
        return json.loads(payload)

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
        """Start buffering outgoing frames for batch write."""
        self._corked = True

    def uncork(self) -> None:
        """Flush buffered frames as a single write and stop buffering."""
        self._flush_cork()
        self._corked = False

    def _flush_cork(self) -> None:
        """Flush corked frames."""
        if not self._cork_buf:
            return
        if _ws_build_frames_batch is not None:
            combined = _ws_build_frames_batch(self._cork_buf)
            self._transport.write(combined)
        else:
            for opcode, payload in self._cork_buf:
                frame = self._build_frame_py(opcode, payload)
                self._transport.write(frame)
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


# ── WebSocket rate limiter ────────────────────────────────────────────────────

class _WsRateLimiter:
    """Token bucket rate limiter for per-connection message throttling."""
    __slots__ = ('_rate', '_burst', '_tokens', '_last_refill')

    def __init__(self, rate: float = 100.0, burst: int = 200):
        self._rate = rate      # messages per second
        self._burst = burst
        self._tokens = float(burst)
        self._last_refill = time.monotonic()

    def allow(self) -> bool:
        now = time.monotonic()
        elapsed = now - self._last_refill
        self._last_refill = now
        self._tokens = min(self._burst, self._tokens + elapsed * self._rate)
        if self._tokens >= 1.0:
            self._tokens -= 1.0
            return True
        return False


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

    __slots__ = ("_core", "_transport", "_buf", "_ka", "_ka_deadline", "_loop", "_wr_paused", "_ws", "_ws_handler", "_ws_ring_buf", "_ws_ping_handle", "_ws_pong_received")

    def __init__(self, core_app: Any, loop: asyncio.AbstractEventLoop) -> None:
        self._core = core_app
        self._loop = loop
        self._transport: asyncio.Transport | None = None
        # Use ring buffer for WebSocket if available, bytearray otherwise (HTTP fallback)
        self._buf = bytearray()
        self._ka: asyncio.TimerHandle | None = None
        self._ka_deadline: float = 0.0
        self._wr_paused = False
        self._ws_handler = None  # Will be set to echo/json/normal handler after upgrade
        self._ws: CppWebSocket | None = None
        self._ws_ring_buf = None  # C++ connection state capsule (created on WebSocket upgrade)
        self._ws_ping_handle: asyncio.TimerHandle | None = None
        self._ws_pong_received = True

    # ── Connection lifecycle ─────────────────────────────────────────────

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self._transport = transport  # type: ignore[assignment]
        sock: socket.socket | None = transport.get_extra_info("socket")  # type: ignore[union-attr]
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                sock.setsockopt(socket.IPPROTO_TCP, 12, 1)  # TCP_QUICKACK = 12
            except (OSError, AttributeError):
                pass
        transport.set_write_buffer_limits(high=131072, low=32768)  # type: ignore[union-attr]
        self._ka_reset()

    def connection_lost(self, exc: Exception | None) -> None:
        self._ka_cancel()
        self._stop_ws_heartbeat()
        if self._ws:
            _server_ws_metrics.active_connections = max(0, _server_ws_metrics.active_connections - 1)
        if self._ws and not self._ws._closed:
            self._ws._closed = True
            self._ws.feed_frame(0x8, b"")  # signal close to waiting receives
        try:
            self._buf.clear()
        except BufferError:
            self._buf = bytearray()  # memoryview still held from crashed data_received
        # Return ring buffer to pool for reuse (or reset and drop)
        if self._ws_ring_buf is not None:
            if _ws_pool is not None:
                _ws_pool.release(self._ws_ring_buf)
            elif _ws_ring_buffer_reset is not None:
                _ws_ring_buffer_reset(self._ws_ring_buf)
            self._ws_ring_buf = None
        self._transport = None

    # ── Data handling — the hot path ─────────────────────────────────────

    def data_received(self, data: bytes) -> None:
        # WebSocket frame handling (after upgrade) — dispatch to mode-specific handler
        if self._ws is not None:
            # Any incoming data means the connection is alive (PONG tracking)
            self._ws_pong_received = True
            self._ws_handler(data)
            return

        buf = self._buf
        buf += data

        if self._wr_paused:
            return

        transport = self._transport
        if not transport or transport.is_closing():
            return

        core = self._core
        IR = _InlineResult

        # Use offset tracking instead of repeated del buf[:consumed]
        # This does ONE memmove at the end instead of N memmoves in the loop
        total_consumed = 0
        mv = memoryview(buf)
        buf_len = len(buf)

        while total_consumed < buf_len:
            # ── C++ handle_http: parse + route + dispatch + write ──────
            try:
                consumed, result = core.handle_http(mv[total_consumed:], transport)
            except Exception:
                mv.release()
                buf.clear()
                transport.write(_500_RESP)
                transport.close()
                return

            if consumed == 0:
                break  # need more data

            if consumed < 0:
                # Parse error — 400 already sent by C++
                mv.release()
                buf.clear()
                transport.close()
                return

            total_consumed += consumed

            # ── Dispatch based on result type ────────────────────────
            if result is True:
                # Sync endpoint handled entirely by C++ — response already written
                continue

            if result is None:
                continue

            if isinstance(result, tuple):
                tag = result[0]
                if tag == "ws":
                    # WebSocket upgrade: ("ws", endpoint, path_params[, path])
                    if len(result) >= 4:
                        _, endpoint, path_params, ws_path = result
                    else:
                        _, endpoint, path_params = result
                        ws_path = "/"
                    # print(f"[WS-DEBUG] upgrade detected: path={ws_path} params={path_params} endpoint={endpoint}", file=sys.stderr)
                    if endpoint is None:
                        # No matching WS route — 101 already sent, just close
                        # print("[WS-DEBUG] endpoint is None — closing", file=sys.stderr)
                        if transport and not transport.is_closing():
                            transport.close()
                        mv.release()
                        buf.clear()
                        return
                    ws = CppWebSocket(
                        transport, self._loop,
                        path=ws_path, path_params=path_params,
                    )
                    ws._protocol = self  # back-ref for echo mode switching
                    ws._channel._protocol = self  # back-ref for backpressure
                    self._ws = ws
                    _server_ws_metrics.active_connections += 1
                    _server_ws_metrics.total_connections += 1
                    self._ka_cancel()  # WS connections are long-lived
                    # TCP_NODELAY: disable Nagle's algorithm for low-latency WS
                    try:
                        sock = transport.get_extra_info("socket")
                        if sock is not None:
                            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    except (OSError, AttributeError):
                        pass
                    # Initialize ring buffer for WebSocket frame accumulation
                    if _ws_pool is not None:
                        self._ws_ring_buf = _ws_pool.acquire()
                    elif _ws_ring_buffer_create is not None:
                        self._ws_ring_buf = _ws_ring_buffer_create()
                    self._ws_handler = self._handle_ws_frames
                    # Start heartbeat to detect dead connections
                    self._start_ws_heartbeat()
                    # print(f"[WS-DEBUG] launching _handle_websocket task", file=sys.stderr)
                    self._loop.create_task(
                        self._handle_websocket(endpoint, path_params))
                    # Remaining buf data (if any) is WebSocket frames
                    remaining = bytes(mv[total_consumed:])
                    mv.release()
                    buf.clear()
                    if remaining:
                        self._ws_handler(bytes(remaining))
                    return  # connection is now WebSocket

                elif tag == "async":
                    # Async endpoint: ("async", coro, status_code, keep_alive)
                    _, coro, status_code, keep_alive = result
                    self._loop.create_task(
                        self._handle_async(coro, status_code, keep_alive))

                elif tag == "async_di":
                    # Async DI: ("async_di", di_coro, endpoint, kwargs, sc, ka)
                    _, di_coro, endpoint, kwargs, sc, ka = result
                    self._loop.create_task(
                        self._handle_async_di(di_coro, endpoint, kwargs, sc, ka))

            elif IR and isinstance(result, IR):
                # Pydantic body validation needed
                self._loop.create_task(self._handle_pydantic(result))

        # Remove all consumed data in one operation (instead of N memmoves)
        mv.release()
        if total_consumed > 0:
            del buf[:total_consumed]

        # ── Flush ─────────────────────────────────────────────────────
        if not buf:
            self._ka_reset()

    # ── WebSocket frame handling ─────────────────────────────────────────

    def _handle_ws_frames(self, data: bytes) -> None:
        """Normal frame handler — single C++ call does everything."""
        ws = self._ws
        if not ws:
            return
        result = _ws_handle_direct(self._ws_ring_buf, data)
        if result is None:
            return
        frames, pong_bytes = result
        transport = self._transport
        if pong_bytes is not None and transport and not transport.is_closing():
            transport.write(pong_bytes)
        _feed_frames(ws, transport, frames, self._ws_ring_buf)

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

    def _handle_ws_frames_json(self, data: bytes) -> None:
        """JSON mode — single C++ call does ring + parse + unmask + JSON decode."""
        ws = self._ws
        if not ws:
            return
        result = _ws_handle_json_direct(self._ws_ring_buf, data)
        if result is None:
            return
        frames, pong_bytes = result
        transport = self._transport
        if pong_bytes is not None and transport and not transport.is_closing():
            transport.write(pong_bytes)
        _feed_frames(ws, transport, frames, self._ws_ring_buf)

    # ── Back-pressure (write flow control) ───────────────────────────────

    def pause_writing(self) -> None:
        self._wr_paused = True

    def resume_writing(self) -> None:
        self._wr_paused = False
        if self._buf and self._transport and not self._transport.is_closing():
            self._loop.call_soon(self._drain_buf)

    def _drain_buf(self) -> None:
        if self._wr_paused or not self._buf:
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
        if _ws_build_ping_frame is not None:
            ping_frame = _ws_build_ping_frame(None)
        else:
            # Python fallback: build PING frame manually
            ping_frame = CppWebSocket._build_frame_py(0x9, b"")
        transport.write(ping_frame)

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
        parts = [f"HTTP/1.1 {sc} OK\r\n".encode()]
        seen = set()
        for k, v in resp_obj.headers.items():
            kl = k.lower()
            seen.add(kl)
            parts.append(f"{k}: {v}\r\n".encode())
        if "content-length" not in seen:
            parts.append(f"content-length: {len(body)}\r\n".encode())
        conn = b"connection: keep-alive\r\n" if keep_alive else b"connection: close\r\n"
        parts.append(conn)
        parts.append(b"\r\n")
        parts.append(body)
        self._transport.write(b"".join(parts))

    # ── Async endpoint dispatch ──────────────────────────────────────────

    async def _handle_async(self, coro: Any, status_code: int, keep_alive: bool) -> None:
        """Await async endpoint coroutine, serialize + write response."""
        try:
            raw = await coro
            if self._transport and not self._transport.is_closing():
                # Handle Starlette Response objects (HTMLResponse, etc.)
                if hasattr(raw, "body") and hasattr(raw, "status_code"):
                    self._write_response_obj(raw, keep_alive)
                else:
                    resp = self._core.build_response(raw, status_code, keep_alive)
                    if resp:
                        self._transport.write(resp)
                    else:
                        self._transport.write(_500_RESP)
        except Exception:
            if self._transport and not self._transport.is_closing():
                self._transport.write(_500_RESP)

    async def _handle_async_di(
        self, di_coro: Any, endpoint: Any, kwargs: dict,
        status_code: int, keep_alive: bool
    ) -> None:
        """Complete async DI resolution, then call endpoint + write response."""
        try:
            # Complete DI resolution
            solved = await di_coro
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
                resp = self._core.build_response(raw, status_code, keep_alive)
                if resp:
                    self._transport.write(resp)
                else:
                    self._transport.write(_500_RESP)
        except Exception:
            if self._transport and not self._transport.is_closing():
                self._transport.write(_500_RESP)

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
                resp = self._core.build_response(raw, status_code, True)
                if resp:
                    self._transport.write(resp)
                else:
                    self._transport.write(_500_RESP)
        except Exception:
            if self._transport and not self._transport.is_closing():
                self._transport.write(_500_RESP)

    # ── WebSocket lifecycle handler ──────────────────────────────────────

    async def _handle_websocket(self, endpoint: Any, path_params: dict) -> None:
        """Run WebSocket endpoint with CppWebSocket wrapper."""
        ws = self._ws
        if not ws:
            return
        try:
            kwargs = dict(path_params) if path_params else {}
            # Inject WebSocket — use cached signature inspection
            ep_id = id(endpoint)
            ws_param = _ws_sig_cache.get(ep_id)
            if ws_param is None:
                sig = inspect.signature(endpoint)
                ws_param = "websocket"  # default
                for param_name, param in sig.parameters.items():
                    ann = param.annotation
                    if param_name == "websocket" or (
                        ann is not inspect.Parameter.empty
                        and getattr(ann, "__name__", "") == "WebSocket"
                    ):
                        ws_param = param_name
                        break
                _ws_sig_cache[ep_id] = ws_param
            kwargs[ws_param] = ws
            await endpoint(**kwargs)
        except Exception as exc:
            ws._metrics.errors += 1
            raise
        finally:
            if not ws._closed:
                await ws.close(1000)
            if self._transport and not self._transport.is_closing():
                self._transport.close()

    # ── Keep-alive timer ─────────────────────────────────────────────────

    def _ka_reset(self) -> None:
        # Update deadline (O(1) — no timer cancel/create)
        self._ka_deadline = time.monotonic() + 15.0
        if not self._ka and self._transport and not self._transport.is_closing():
            self._ka = self._loop.call_later(15.0, self._ka_check)

    def _ka_cancel(self) -> None:
        h = self._ka
        if h is not None:
            h.cancel()
            self._ka = None

    def _ka_check(self) -> None:
        """Check if deadline passed; reschedule if not."""
        self._ka = None
        remaining = self._ka_deadline - time.monotonic()
        if remaining <= 0:
            if self._transport and not self._transport.is_closing():
                self._transport.close()
        elif self._transport and not self._transport.is_closing():
            self._ka = self._loop.call_later(remaining, self._ka_check)


# ═════════════════════════════════════════════════════════════════════════════
# Server startup
# ═════════════════════════════════════════════════════════════════════════════

async def run_server(
    app: Any, host: str = "127.0.0.1", port: int = 8000
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

    loop = asyncio.get_event_loop()

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

    # Freeze routes — skip shared_lock in hot path after startup
    if hasattr(core_app, "freeze_routes"):
        core_app.freeze_routes()

    # ── Cache OpenAPI schema as pre-built HTTP response ──────────────────
    if hasattr(core_app, "set_openapi_schema") and hasattr(app, "openapi"):
        try:
            import json
            schema = app.openapi()
            if schema:
                schema_json = json.dumps(schema, ensure_ascii=False, separators=(",", ":"))
                core_app.set_openapi_schema(schema_json)
        except Exception:
            pass  # Non-fatal — /docs won't work but server still runs

    # ── Lifespan: startup events ─────────────────────────────────────────
    router = getattr(app, "router", None)
    if router:
        for handler in getattr(router, "on_startup", []):
            if asyncio.iscoroutinefunction(handler):
                await handler()
            else:
                handler()

    server = await loop.create_server(
        lambda: CppHttpProtocol(core_app, loop),
        host,
        port,
        reuse_address=True,
        backlog=2048,
    )

    print(f"C++ HTTP server running on http://{host}:{port}")
    print("Press Ctrl+C to stop")

    stop_event = asyncio.Event()

    def _signal_handler() -> None:
        print("\nShutting down...")
        stop_event.set()

    if sys.platform != "win32":
        loop.add_signal_handler(signal.SIGINT, _signal_handler)
        loop.add_signal_handler(signal.SIGTERM, _signal_handler)

    try:
        async with server:
            if sys.platform == "win32":
                await server.serve_forever()
            else:
                await stop_event.wait()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
        await server.wait_closed()

        # ── Lifespan: shutdown events ────────────────────────────────
        if router:
            for handler in getattr(router, "on_shutdown", []):
                if asyncio.iscoroutinefunction(handler):
                    await handler()
                else:
                    handler()

        print("Server stopped.")
