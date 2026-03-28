"""
Native exception handler wrapper -- replaces starlette._exception_handler.

Wraps an ASGI app so that registered exception handlers (keyed by exception
class or HTTP status code) are invoked when an exception propagates out of
the inner app.  This is used by ``request_response`` and ``websocket_session``
in the routing layer.
"""
from __future__ import annotations

from typing import Any

from astraapi._concurrency import is_async_callable, run_in_threadpool
from astraapi._types import ASGIApp, Receive, Scope, Send

# Type aliases matching Starlette conventions
ExceptionHandler = Any
ExceptionHandlers = dict[Any, ExceptionHandler]
StatusHandlers = dict[int, ExceptionHandler]

# Message type (ASGI send messages)
Message = dict[str, Any]


def _lookup_exception_handler(
    exc_handlers: ExceptionHandlers, exc: Exception
) -> ExceptionHandler | None:
    """Walk the MRO of the exception to find a matching handler."""
    for cls in type(exc).__mro__:
        if cls in exc_handlers:
            return exc_handlers[cls]
    return None


def wrap_app_handling_exceptions(
    app: ASGIApp, conn: Any
) -> ASGIApp:
    """Wrap an ASGI app to catch exceptions and dispatch to registered handlers.

    Parameters
    ----------
    app : ASGIApp
        The inner ASGI application to wrap.
    conn : Request | WebSocket
        The connection object whose scope holds the exception handler
        registry at ``scope["starlette.exception_handlers"]``.

    Returns
    -------
    ASGIApp
        A new ASGI callable that intercepts exceptions and invokes the
        appropriate handler if one is registered.
    """
    # Eagerly read handlers from the scope so the lookup is done once.
    exception_handlers: ExceptionHandlers
    status_handlers: StatusHandlers
    try:
        exception_handlers, status_handlers = conn.scope["starlette.exception_handlers"]
    except KeyError:
        exception_handlers, status_handlers = {}, {}

    # Cache HTTPException import once at wrapping time, not per-exception
    try:
        from astraapi.exceptions import HTTPException as _HTTPException
    except ImportError:
        _HTTPException = None  # type: ignore[misc, assignment]

    async def wrapped_app(scope: Scope, receive: Receive, send: Send) -> None:
        response_started = False

        async def sender(message: Message) -> None:
            nonlocal response_started
            if message["type"] == "http.response.start":
                response_started = True
            await send(message)

        try:
            await app(scope, receive, sender)
        except Exception as exc:
            handler = None

            HTTPException = _HTTPException

            if HTTPException is not None and isinstance(exc, HTTPException):
                handler = status_handlers.get(exc.status_code)

            # Then try matching by exception class via MRO walk
            if handler is None:
                handler = _lookup_exception_handler(exception_handlers, exc)

            if handler is None:
                raise exc

            if response_started:
                raise RuntimeError(
                    "Caught handled exception, but response already started."
                ) from exc

            if is_async_callable(handler):
                response = await handler(conn, exc)
            else:
                response = await run_in_threadpool(handler, conn, exc)
            if response is not None:
                await response(scope, receive, sender)

    return wrapped_app
