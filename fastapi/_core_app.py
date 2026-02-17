"""
Thin Python async wrapper for CoreApp ASGI core.

Python handles ONLY unavoidable async I/O + user endpoint calls.
Everything else is C++ core.
"""

from __future__ import annotations

from typing import Optional

from fastapi.exceptions import HTTPException
from fastapi._response import Response
from fastapi._types import Receive, Scope, Send

from fastapi._core_bridge import (
    CoreApp,
    encode_to_json_bytes,
)

# ── Pre-built ASGI constants (ZERO per-request allocation) ───────────────────

_CT_JSON = [b"content-type", b"application/json"]

_500_START = {"type": "http.response.start", "status": 500, "headers": [_CT_JSON]}
_500_BODY = {"type": "http.response.body", "body": b'{"detail":"Internal Server Error"}'}
_422_START = {"type": "http.response.start", "status": 422, "headers": [_CT_JSON]}


async def _read_body(receive: Receive) -> bytes:
    chunks: list[bytes] = []
    while True:
        msg = await receive()
        chunk = msg.get("body", b"")
        if chunk:
            chunks.append(chunk)
        if not msg.get("more_body", False):
            break
    if not chunks:
        return b""
    return chunks[0] if len(chunks) == 1 else b"".join(chunks)


async def _resume_coro(coro, first_yield):
    """Resume a coroutine that already yielded once (endpoint with real I/O).

    C++ drove the first step via PyIter_Send; this finishes the rest.
    """
    result = await first_yield
    try:
        while True:
            result = await coro.send(result)
    except StopIteration as e:
        return e.value


class CoreASGIApp:
    """Minimal wrapper — provides handle_fast() for the C++ fast path.

    Lifespan, WebSocket, and non-fast HTTP are handled by Starlette.
    """

    __slots__ = ("_core_app", "_fast_routes_registered", "_handle", "_build")

    def __init__(self, core_app: CoreApp) -> None:
        self._core_app = core_app
        self._fast_routes_registered: bool = False
        self._handle = core_app.handle_and_respond   # cached bound method
        self._build = core_app.build_asgi_response    # fallback for suspended coro

    @property
    def core_app(self) -> CoreApp:
        return self._core_app

    # ── FAST PATH ────────────────────────────────────────────────────────────
    #
    # Single-call architecture: scope dict + body → C++ does EVERYTHING:
    # - Route matching
    # - Parameter extraction (string_view, pre-interned keys)
    # - Endpoint invocation via PyIter_Send
    # - JSON serialization
    # - ASGI dict building
    #
    # Returns:
    #   tuple(start, body)  len==2 → fully handled, just send
    #   tuple(-1, coro, sc) len==3 → endpoint suspended, await wrapper
    #   InlineResult                → needs Pydantic body validation
    #   Response                    → Starlette Response
    #   None                        → no route match

    async def handle_fast(
        self, scope: Scope, receive: Receive, send: Send
    ) -> bool:
        if not self._fast_routes_registered:
            return False

        # Read body for POST/PUT/PATCH (must happen in Python — ASGI async I/O)
        body_bytes: Optional[bytes] = None
        method = scope["method"]
        if method == "POST" or method == "PUT" or method == "PATCH":
            body_bytes = await _read_body(receive)

        # ── SINGLE C++ call ────────────────────────────────────────────────
        result = self._handle(scope, body_bytes)

        if result is None:
            return False

        # ── Dispatch on result type ────────────────────────────────────────
        if type(result) is tuple:
            n = len(result)
            if n == 2:
                # FAST PATH: C++ handled everything (route + params + endpoint + JSON + ASGI)
                await send(result[0])
                await send(result[1])
                return True
            if n == 3:
                # Endpoint suspended — C++ started the coroutine, Python finishes
                raw = await result[1]
                if isinstance(raw, Response):
                    await raw(scope, receive, send)
                    return True
                start, body_msg = self._build(raw, result[2])
                await send(start)
                await send(body_msg)
                return True

        # ── InlineResult — has body params, needs Pydantic validation ──────
        r = result
        if r.has_body_params and r.json_body is not None:
            from fastapi.dependencies.utils import request_body_to_args
            body_values, body_errors = await request_body_to_args(
                body_fields=r.body_params,
                received_body=r.json_body,
                embed_body_fields=r.embed_body_fields,
            )
            if body_errors:
                eb = encode_to_json_bytes({"detail": body_errors})
                await send(_422_START)
                await send({"type": "http.response.body", "body": eb})
                return True
            r.kwargs.update(body_values)

        # ── Call endpoint (InlineResult path — body params needed Pydantic) ─
        try:
            raw = await r.endpoint(**r.kwargs)
        except HTTPException as exc:
            start, body_msg = self._build({"detail": exc.detail}, exc.status_code)
            await send(start)
            await send(body_msg)
            return True
        except Exception:
            await send(_500_START)
            await send(_500_BODY)
            return True

        if isinstance(raw, Response):
            await raw(scope, receive, send)
            return True

        # ── Serialize + build ASGI dicts ───────────────────────────────────
        start, body_msg = self._build(raw, r.status_code)
        await send(start)
        await send(body_msg)
        return True
