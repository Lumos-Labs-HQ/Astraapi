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
import hashlib
import signal
import socket
import struct
import sys
import time
from typing import Any

# C++ WebSocket unmask (8-byte-at-a-time XOR, ~10x faster than Python loop)
try:
    from _fastapi_core import ws_unmask as _ws_unmask
except ImportError:
    _ws_unmask = None

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

    __slots__ = ("_transport", "_loop", "_recv_queue", "_closed", "_close_code")

    def __init__(self, transport: asyncio.Transport, loop: asyncio.AbstractEventLoop) -> None:
        self._transport = transport
        self._loop = loop
        self._recv_queue: asyncio.Queue[tuple[int, bytes]] = asyncio.Queue()
        self._closed = False
        self._close_code = 1000

    async def accept(self) -> None:
        """Accept the WebSocket connection (upgrade already sent by C++)."""
        pass  # 101 already sent by C++ handle_http

    async def send_text(self, data: str) -> None:
        """Send a text message."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        payload = data.encode("utf-8")
        frame = self._build_frame(0x1, payload)
        self._transport.write(frame)

    async def send_bytes(self, data: bytes) -> None:
        """Send a binary message."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        frame = self._build_frame(0x2, data)
        self._transport.write(frame)

    async def send_json(self, data: Any) -> None:
        """Send JSON data as text."""
        import json
        await self.send_text(json.dumps(data))

    async def receive_text(self) -> str:
        """Receive a text message."""
        opcode, payload = await self._recv_queue.get()
        if opcode == 0x8:  # close
            self._closed = True
            raise RuntimeError("WebSocket closed by client")
        return payload.decode("utf-8")

    async def receive_bytes(self) -> bytes:
        """Receive a binary message."""
        opcode, payload = await self._recv_queue.get()
        if opcode == 0x8:
            self._closed = True
            raise RuntimeError("WebSocket closed by client")
        return payload

    async def receive_json(self) -> Any:
        """Receive and parse JSON data."""
        import json
        text = await self.receive_text()
        return json.loads(text)

    async def close(self, code: int = 1000) -> None:
        """Send close frame and close connection."""
        if not self._closed:
            self._closed = True
            self._close_code = code
            # Build close frame: code as 2 bytes
            payload = struct.pack("!H", code)
            frame = self._build_frame(0x8, payload)
            try:
                self._transport.write(frame)
            except Exception:
                pass

    def feed_frame(self, opcode: int, payload: bytes) -> None:
        """Feed a parsed frame from the protocol handler."""
        self._recv_queue.put_nowait((opcode, payload))

    @staticmethod
    def _build_frame(opcode: int, payload: bytes) -> bytes:
        """Build an unmasked server→client WebSocket frame."""
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

    __slots__ = ("_core", "_transport", "_buf", "_ka", "_ka_deadline", "_loop", "_wr_paused", "_ws")

    def __init__(self, core_app: Any, loop: asyncio.AbstractEventLoop) -> None:
        self._core = core_app
        self._loop = loop
        self._transport: asyncio.Transport | None = None
        self._buf = bytearray()
        self._ka: asyncio.TimerHandle | None = None
        self._ka_deadline: float = 0.0
        self._wr_paused = False
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
        self._buf.clear()
        self._transport = None

    # ── Data handling — the hot path ─────────────────────────────────────

    def data_received(self, data: bytes) -> None:
        # WebSocket frame handling (after upgrade)
        if self._ws is not None:
            self._handle_ws_frames(data)
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
                buf.clear()
                transport.write(_500_RESP)
                transport.close()
                return

            if consumed == 0:
                break  # need more data

            if consumed < 0:
                # Parse error — 400 already sent by C++
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
                    # WebSocket upgrade: ("ws", endpoint, path_params)
                    _, endpoint, path_params = result
                    self._ws = CppWebSocket(transport, self._loop)
                    self._loop.create_task(
                        self._handle_websocket(endpoint, path_params))
                    # Remaining buf data (if any) is WebSocket frames
                    remaining = buf[total_consumed:]
                    buf.clear()
                    if remaining:
                        self._handle_ws_frames(bytes(remaining))
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
        """Parse incoming WebSocket frames and feed to CppWebSocket."""
        buf = self._buf
        buf += data
        ws = self._ws
        if not ws:
            return

        while len(buf) >= 2:
            # Parse frame header
            byte0 = buf[0]
            byte1 = buf[1]
            fin = bool(byte0 & 0x80)
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

            # Extract payload
            payload = bytearray(buf[pos:pos + payload_len])
            if mask:
                if _ws_unmask is not None:
                    _ws_unmask(payload, bytes(mask))
                else:
                    for i in range(payload_len):
                        payload[i] ^= mask[i & 3]

            del buf[:pos + payload_len]

            # Handle control frames
            if opcode == 0x8:  # Close
                ws.feed_frame(0x8, bytes(payload))
                # Send close frame back
                close_frame = CppWebSocket._build_frame(0x8, bytes(payload[:2]) if len(payload) >= 2 else b"\x03\xe8")
                if self._transport and not self._transport.is_closing():
                    self._transport.write(close_frame)
                return
            elif opcode == 0x9:  # Ping → send Pong
                pong = CppWebSocket._build_frame(0xA, bytes(payload))
                if self._transport and not self._transport.is_closing():
                    self._transport.write(pong)
                continue
            elif opcode == 0xA:  # Pong — ignore
                continue

            # Data frame (text or binary)
            ws.feed_frame(opcode, bytes(payload))

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

    # ── Async endpoint dispatch ──────────────────────────────────────────

    async def _handle_async(self, coro: Any, status_code: int, keep_alive: bool) -> None:
        """Await async endpoint coroutine, serialize + write response."""
        try:
            raw = await coro
            if self._transport and not self._transport.is_closing():
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
            kwargs["websocket"] = ws
            await endpoint(**kwargs)
        except Exception:
            pass
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
