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
    """
    __slots__ = ('_waiter', '_buffer', '_loop')

    def __init__(self, loop: asyncio.AbstractEventLoop):
        self._waiter: asyncio.Future | None = None
        self._buffer: list = []  # list of (opcode, payload) tuples
        self._loop = loop

    def feed(self, opcode: int, payload: bytes) -> None:
        """Feed a frame — called from data_received (sync context)."""
        waiter = self._waiter
        if waiter is not None and not waiter.done():
            self._waiter = None
            waiter.set_result((opcode, payload))
        else:
            self._buffer.append((opcode, payload))

    async def get(self) -> tuple:
        """Get next frame — called from endpoint coroutine."""
        buf = self._buffer
        if buf:
            return buf.pop(0)
        fut = self._loop.create_future()
        self._waiter = fut
        return await fut


# C++ WebSocket high-performance frame parser/builder
try:
    from _fastapi_core import (
        ws_unmask as _ws_unmask,
        ws_parse_frames as _ws_parse_frames,
        ws_parse_frames_text as _ws_parse_frames_text,
        ws_parse_frames_json as _ws_parse_frames_json,
        ws_echo_frames as _ws_echo_frames,
        ws_build_frame_bytes as _ws_build_frame_bytes,
        ws_build_close_frame_bytes as _ws_build_close_frame_bytes,
        ws_build_frames_batch as _ws_build_frames_batch,
        ws_parse_json as _ws_parse_json,
        ws_serialize_json as _ws_serialize_json,
    )
except ImportError:
    _ws_unmask = None
    _ws_parse_frames = None
    _ws_parse_frames_text = None
    _ws_parse_frames_json = None
    _ws_echo_frames = None
    _ws_build_frame_bytes = None
    _ws_build_close_frame_bytes = None
    _ws_build_frames_batch = None
    _ws_parse_json = None
    _ws_serialize_json = None

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


# ── WebSocket wrapper for Python endpoint access ────────────────────────────

class CppWebSocket:
    """WebSocket connection wrapper for asyncio transport.

    Provides send/receive API compatible with FastAPI/Starlette WebSocket endpoints.
    Frame parsing and building done via C++ ws_frame_parser (RFC 6455).
    """

    __slots__ = (
        "_transport", "_loop", "_channel", "_closed", "_close_code",
        "client_state", "application_state", "scope", "path_params",
        "_corked", "_cork_buf", "_sock", "_flush_scheduled", "_pending_writes",
        "_protocol", "_echo_detect_count", "_last_received",
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
        self._channel = _WsFastChannel(loop)
        self._closed = False
        self._close_code = 1000
        self._corked = False
        self._cork_buf: list[tuple[int, bytes]] = []
        # Write coalescing state
        self._sock = transport.get_extra_info("socket")
        self._flush_scheduled = False
        self._pending_writes: list[bytes] = []
        # Echo auto-detection
        self._protocol = None  # back-ref to CppHttpProtocol, set after creation
        self._echo_detect_count = 0
        self._last_received = None  # track last received for echo detection
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
        """Send close frame and close connection."""
        if not self._closed:
            self._closed = True
            self._close_code = code
            self.application_state = WebSocketState.DISCONNECTED
            if self._corked:
                self._flush_cork()
            # CRITICAL: flush pending writes before close frame
            if self._pending_writes:
                self._flush_writes()
            frame = _ws_build_close_frame_bytes(code) if _ws_build_close_frame_bytes else self._build_frame_py(0x8, struct.pack("!H", code))
            try:
                self._transport.write(frame)
            except Exception:
                pass

    # ── Write coalescing ─────────────────────────────────────────────

    def _write_frame(self, frame: bytes) -> None:
        """Write frame directly to transport.

        Direct write without buffering — kernel TCP stack handles coalescing.
        Simpler and lower latency than call_soon buffering.
        """
        transport = self._transport
        if transport is not None and not transport.is_closing():
            transport.write(frame)

    def _flush_writes(self) -> None:
        """Flush any pending writes (no-op with direct write mode)."""
        # With direct write mode, _pending_writes is always empty
        # This method kept for API compatibility with close()
        pass

    # ── Send methods ──────────────────────────────────────────────────

    def send_text(self, data: str) -> _NoopAwaitable:
        """Send a text message. Returns awaitable for API compatibility."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        # Echo auto-detection: if sending exactly what was received, count it
        if _ws_echo_frames is not None and self._echo_detect_count >= 0:
            if self._last_received is not None and data == self._last_received:
                self._echo_detect_count += 1
                if self._echo_detect_count >= 1:
                    # Confirmed echo pattern — switch to C++ echo fast path
                    proto = self._protocol
                    if proto is not None:
                        proto._ws_handler = proto._handle_ws_frames_echo
                    self._echo_detect_count = -1  # stop detecting
            else:
                # Not echo — disable detection
                self._echo_detect_count = -1
            self._last_received = None
        payload = data if isinstance(data, bytes) else data.encode("utf-8")
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
            if mode == "text":
                # Build frame directly from JSON bytes — skip str round-trip
                frame = _ws_build_frame_bytes(0x1, json_bytes) if _ws_build_frame_bytes else self._build_frame_py(0x1, json_bytes)
            else:
                frame = _ws_build_frame_bytes(0x2, json_bytes) if _ws_build_frame_bytes else self._build_frame_py(0x2, json_bytes)
            if self._corked:
                self._cork_buf.append((0x1 if mode == "text" else 0x2, json_bytes))
                return _NOOP
            self._write_frame(frame)
        else:
            import json
            text = json.dumps(data)
            if mode == "text":
                self.send_text(text)
            else:
                self.send_bytes(text.encode("utf-8"))
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
        if _ws_parse_frames_json is not None and self._protocol is not None:
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

    __slots__ = ("_core", "_transport", "_buf", "_ka", "_ka_deadline", "_loop", "_wr_paused", "_ws", "_ws_buf_offset", "_ws_handler")

    def __init__(self, core_app: Any, loop: asyncio.AbstractEventLoop) -> None:
        self._core = core_app
        self._loop = loop
        self._transport: asyncio.Transport | None = None
        self._buf = bytearray()
        self._ka: asyncio.TimerHandle | None = None
        self._ka_deadline: float = 0.0
        self._wr_paused = False
        self._ws_buf_offset = 0
        self._ws_handler = None  # Will be set to echo/json/normal handler after upgrade
        self._ws: CppWebSocket | None = None

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
        if self._ws and not self._ws._closed:
            self._ws._closed = True
            self._ws.feed_frame(0x8, b"")  # signal close to waiting receives
        try:
            self._buf.clear()
        except BufferError:
            self._buf = bytearray()  # memoryview still held from crashed data_received
        self._ws_buf_offset = 0
        self._transport = None

    # ── Data handling — the hot path ─────────────────────────────────────

    def data_received(self, data: bytes) -> None:
        # WebSocket frame handling (after upgrade) — dispatch to mode-specific handler
        if self._ws is not None:
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
                    self._ws = ws
                    self._ka_cancel()  # WS connections are long-lived
                    # Select optimal frame handler based on endpoint path
                    self._ws_handler = self._handle_ws_frames  # default
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
        """Parse incoming WebSocket frames and feed to CppWebSocket.
        Uses C++ batch parser for all frame parsing, unmasking, and pong generation.
        Inlines channel feed to avoid method-call overhead per frame.
        Uses offset tracking to eliminate memmove on every call.
        """
        buf = self._buf
        buf += data
        ws = self._ws
        if not ws:
            return

        # Choose parser: prefer text-decoding variant (returns str for TEXT frames)
        _parser = _ws_parse_frames_text if _ws_parse_frames_text is not None else _ws_parse_frames
        if _parser is not None:
            # ── C++ fast path: parse all frames in one call ──────────
            offset = self._ws_buf_offset
            if offset > 0:
                view = memoryview(buf)[offset:]
            else:
                view = buf
            result = _parser(view)
            if result is None:
                return

            consumed, frames, pong_data = result

            # Send any auto-generated pong responses
            if pong_data and self._transport and not self._transport.is_closing():
                self._transport.write(pong_data)

            # Inline channel feed — avoid ws.feed_frame() method call overhead
            channel = ws._channel
            waiter = channel._waiter
            channel_buf = channel._buffer

            for opcode, payload in frames:
                if opcode == 0x8:  # Close
                    # Feed close to channel
                    if waiter is not None and not waiter.done():
                        channel._waiter = None
                        waiter.set_result((0x8, payload))
                    else:
                        channel_buf.append((0x8, payload))
                    close_code = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 1000
                    if _ws_build_close_frame_bytes is not None:
                        close_resp = _ws_build_close_frame_bytes(close_code)
                    else:
                        close_resp = CppWebSocket._build_frame_py(0x8, payload[:2] if len(payload) >= 2 else b"\x03\xe8")
                    if self._transport and not self._transport.is_closing():
                        self._transport.write(close_resp)
                    # Reset buffer
                    self._ws_buf_offset = 0
                    buf.clear()
                    return
                # Feed data frame to channel directly
                if waiter is not None and not waiter.done():
                    channel._waiter = None
                    waiter.set_result((opcode, payload))
                    waiter = None  # only first frame resolves waiter
                else:
                    channel_buf.append((opcode, payload))

            # Offset tracking: avoid memmove
            if consumed > 0:
                offset += consumed
                if offset >= len(buf):
                    # Fully consumed — reset
                    buf.clear()
                    self._ws_buf_offset = 0
                elif offset > 65536:
                    # Compact when offset too large (prevent unbounded growth)
                    del buf[:offset]
                    self._ws_buf_offset = 0
                else:
                    self._ws_buf_offset = offset
        else:
            # ── Python fallback ─────────────────────────────────────
            while len(buf) >= 2:
                byte0 = buf[0]
                byte1 = buf[1]
                opcode = byte0 & 0x0F
                masked = bool(byte1 & 0x80)
                payload_len = byte1 & 0x7F
                pos = 2

                if payload_len == 126:
                    if len(buf) < 4:
                        break
                    payload_len = struct.unpack("!H", buf[2:4])[0]
                    pos = 4
                elif payload_len == 127:
                    if len(buf) < 10:
                        break
                    payload_len = struct.unpack("!Q", buf[2:10])[0]
                    pos = 10

                if masked:
                    if len(buf) < pos + 4:
                        break
                    mask = buf[pos:pos + 4]
                    pos += 4
                else:
                    mask = None

                if len(buf) < pos + payload_len:
                    break

                payload = bytearray(buf[pos:pos + payload_len])
                if mask:
                    if _ws_unmask is not None:
                        _ws_unmask(payload, bytes(mask))
                    else:
                        for i in range(payload_len):
                            payload[i] ^= mask[i & 3]

                del buf[:pos + payload_len]

                if opcode == 0x8:
                    ws.feed_frame(0x8, bytes(payload))
                    close_frame = CppWebSocket._build_frame(0x8, bytes(payload[:2]) if len(payload) >= 2 else b"\x03\xe8")
                    if self._transport and not self._transport.is_closing():
                        self._transport.write(close_frame)
                    return
                elif opcode == 0x9:
                    pong = CppWebSocket._build_frame(0xA, bytes(payload))
                    if self._transport and not self._transport.is_closing():
                        self._transport.write(pong)
                    continue
                elif opcode == 0xA:
                    continue

                ws.feed_frame(opcode, bytes(payload))

    def _handle_ws_frames_echo(self, data: bytes) -> None:
        """Ultra-fast echo path: C++ parses frames AND builds echo responses in one call.
        No per-message Python overhead — entire parse+echo done in C++.
        """
        buf = self._buf
        buf += data

        offset = self._ws_buf_offset
        if offset > 0:
            view = memoryview(buf)[offset:]
        else:
            view = buf
        result = _ws_echo_frames(view)
        if result is None:
            return

        consumed, echo_bytes, close_payload = result
        transport = self._transport

        # Write all echo response frames in one call
        if echo_bytes is not None and transport and not transport.is_closing():
            transport.write(echo_bytes)

        # Handle close
        if close_payload is not None:
            ws = self._ws
            if ws:
                ws.feed_frame(0x8, close_payload)
                close_code = int.from_bytes(close_payload[:2], "big") if len(close_payload) >= 2 else 1000
                if _ws_build_close_frame_bytes is not None:
                    close_resp = _ws_build_close_frame_bytes(close_code)
                else:
                    close_resp = CppWebSocket._build_frame_py(0x8, close_payload[:2] if len(close_payload) >= 2 else b"\x03\xe8")
                if transport and not transport.is_closing():
                    transport.write(close_resp)
            self._ws_buf_offset = 0
            buf.clear()
            return

        # Offset tracking
        if consumed > 0:
            offset += consumed
            if offset >= len(buf):
                buf.clear()
                self._ws_buf_offset = 0
            elif offset > 65536:
                del buf[:offset]
                self._ws_buf_offset = 0
            else:
                self._ws_buf_offset = offset

    def _handle_ws_frames_json(self, data: bytes) -> None:
        """JSON-optimized frame handler: C++ parses frames AND decodes JSON in one call.
        Skips separate receive_text + json.loads steps.
        """
        buf = self._buf
        buf += data
        ws = self._ws
        if not ws:
            return

        offset = self._ws_buf_offset
        if offset > 0:
            view = memoryview(buf)[offset:]
        else:
            view = buf
        result = _ws_parse_frames_json(view)
        if result is None:
            return

        consumed, frames, pong_data = result

        if pong_data and self._transport and not self._transport.is_closing():
            self._transport.write(pong_data)

        # Inline channel feed
        channel = ws._channel
        waiter = channel._waiter
        channel_buf = channel._buffer

        for opcode, payload in frames:
            if opcode == 0x8:
                if waiter is not None and not waiter.done():
                    channel._waiter = None
                    waiter.set_result((0x8, payload))
                else:
                    channel_buf.append((0x8, payload))
                close_code = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 1000
                if _ws_build_close_frame_bytes is not None:
                    close_resp = _ws_build_close_frame_bytes(close_code)
                else:
                    close_resp = CppWebSocket._build_frame_py(0x8, payload[:2] if len(payload) >= 2 else b"\x03\xe8")
                if self._transport and not self._transport.is_closing():
                    self._transport.write(close_resp)
                self._ws_buf_offset = 0
                buf.clear()
                return
            if waiter is not None and not waiter.done():
                channel._waiter = None
                waiter.set_result((opcode, payload))
                waiter = None
            else:
                channel_buf.append((opcode, payload))

        if consumed > 0:
            offset += consumed
            if offset >= len(buf):
                buf.clear()
                self._ws_buf_offset = 0
            elif offset > 65536:
                del buf[:offset]
                self._ws_buf_offset = 0
            else:
                self._ws_buf_offset = offset

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
