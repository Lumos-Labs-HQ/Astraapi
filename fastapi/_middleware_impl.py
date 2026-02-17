"""
Middleware implementations -- replaces multiple starlette.middleware modules.

Provides:
    Middleware              (starlette.middleware.Middleware)
    ServerErrorMiddleware   (starlette.middleware.errors.ServerErrorMiddleware)
    ExceptionMiddleware     (starlette.middleware.exceptions.ExceptionMiddleware)
    BaseHTTPMiddleware      (starlette.middleware.base.BaseHTTPMiddleware)
    CORSMiddleware          (starlette.middleware.cors.CORSMiddleware)
    GZipMiddleware          (starlette.middleware.gzip.GZipMiddleware)
    TrustedHostMiddleware   (starlette.middleware.trustedhost.TrustedHostMiddleware)
    HTTPSRedirectMiddleware (starlette.middleware.httpsredirect.HTTPSRedirectMiddleware)
    WSGIMiddleware          (starlette.middleware.wsgi.WSGIMiddleware)

All with zero starlette imports.
"""
from __future__ import annotations

import asyncio
import concurrent.futures
import gzip
import html
import io
import re
import sys
import traceback
from typing import (
    Any,
    Callable,
    Dict,
    List,
    Optional,
    Sequence,
    Set,
    Tuple,
    Type,
    Union,
)

from fastapi._types import ASGIApp, Receive, Scope, Send
from fastapi._concurrency import run_in_threadpool, is_async_callable
from fastapi._request import Request
from fastapi._response import (
    Response,
    PlainTextResponse,
    HTMLResponse,
    RedirectResponse,
)


# ---------------------------------------------------------------------------
# Middleware descriptor
# ---------------------------------------------------------------------------


class Middleware:
    """Descriptor for a middleware class together with its keyword arguments.

    Used by application classes to build middleware stacks declaratively:

        middleware = [
            Middleware(CORSMiddleware, allow_origins=["*"]),
        ]
    """

    def __init__(self, cls: type, *args: Any, **kwargs: Any) -> None:
        self.cls = cls
        self.args = args
        self.kwargs = kwargs

    def __repr__(self) -> str:
        class_name = getattr(self.cls, "__name__", repr(self.cls))
        args_strings = [repr(value) for value in self.args]
        option_strings = [f"{key}={value!r}" for key, value in self.kwargs.items()]
        args_repr = ", ".join([class_name] + args_strings + option_strings)
        return f"Middleware({args_repr})"

    def __iter__(self):
        return iter((self.cls, self.args, self.kwargs))


# ---------------------------------------------------------------------------
# ServerErrorMiddleware
# ---------------------------------------------------------------------------


_500_PLAIN = "Internal Server Error"

_500_DEBUG_HTML = """<!DOCTYPE html>
<html>
<head><title>500 Internal Server Error</title>
<style>
body {{ font-family: monospace; padding: 2em; }}
pre {{ background: #f4f4f4; padding: 1em; overflow-x: auto; }}
h1 {{ color: #c00; }}
</style>
</head>
<body>
<h1>500 Internal Server Error</h1>
<pre>{traceback}</pre>
</body>
</html>
"""


class ServerErrorMiddleware:
    """ASGI middleware that catches unhandled exceptions and returns a
    500 Internal Server Error response.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    handler : callable or None
        Custom error handler ``async handler(request, exc) -> Response``.
    debug : bool
        If ``True``, include the full traceback in the response.
    """

    def __init__(
        self,
        app: ASGIApp,
        handler: Optional[Callable[..., Any]] = None,
        debug: bool = False,
    ) -> None:
        self.app = app
        self.handler = handler
        self.debug = debug

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        response_started = False
        original_send = send

        async def sender(message: Any) -> None:
            nonlocal response_started
            if message["type"] == "http.response.start":
                response_started = True
            await original_send(message)

        try:
            await self.app(scope, receive, sender)
        except Exception as exc:
            if response_started:
                raise
            request = Request(scope, receive, send)
            if self.handler is not None:
                if is_async_callable(self.handler):
                    response = await self.handler(request, exc)
                else:
                    response = await run_in_threadpool(self.handler, request, exc)
            elif self.debug:
                tb = traceback.format_exception(type(exc), exc, exc.__traceback__)
                tb_text = html.escape("".join(tb))
                content = _500_DEBUG_HTML.format(traceback=tb_text)
                response = HTMLResponse(content, status_code=500)
            else:
                response = PlainTextResponse(_500_PLAIN, status_code=500)
            await response(scope, receive, send)


# ---------------------------------------------------------------------------
# ExceptionMiddleware
# ---------------------------------------------------------------------------


class ExceptionMiddleware:
    """ASGI middleware that dispatches to registered exception handlers.

    For HTTP requests, catches exceptions and looks up a handler by
    walking the exception's MRO.  Also supports integer status code
    handlers for ``HTTPException``-style handling.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    handlers : dict or None
        Mapping of exception class (or int status code) to handler callables.
    debug : bool
        Passed through for informational purposes.
    """

    def __init__(
        self,
        app: ASGIApp,
        handlers: Optional[Dict[Any, Callable[..., Any]]] = None,
        debug: bool = False,
    ) -> None:
        self.app = app
        self.debug = debug
        self._exception_handlers: Dict[Any, Callable[..., Any]] = {}
        self._status_handlers: Dict[int, Callable[..., Any]] = {}
        if handlers:
            for key, handler in handlers.items():
                self.add_exception_handler(key, handler)

    def add_exception_handler(
        self,
        exc_class_or_status: Union[int, Type[Exception]],
        handler: Callable[..., Any],
    ) -> None:
        """Register an exception handler.

        Parameters
        ----------
        exc_class_or_status :
            An exception class or an HTTP status code integer.
        handler :
            ``async handler(request, exc) -> Response``
        """
        if isinstance(exc_class_or_status, int):
            self._status_handlers[exc_class_or_status] = handler
        else:
            self._exception_handlers[exc_class_or_status] = handler

    def _lookup_handler(self, exc: Exception) -> Optional[Callable[..., Any]]:
        """Walk the MRO of *exc* to find a registered handler."""
        for cls in type(exc).__mro__:
            if cls in self._exception_handlers:
                return self._exception_handlers[cls]
        return None

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        response_started = False
        original_send = send

        async def sender(message: Any) -> None:
            nonlocal response_started
            if message["type"] == "http.response.start":
                response_started = True
            await original_send(message)

        try:
            await self.app(scope, receive, sender)
        except Exception as exc:
            if response_started:
                raise

            request = Request(scope, receive, send)

            # Check for status-code based handler (HTTPException pattern)
            status_code = getattr(exc, "status_code", None)
            if status_code is not None and status_code in self._status_handlers:
                handler = self._status_handlers[status_code]
                if is_async_callable(handler):
                    response = await handler(request, exc)
                else:
                    response = await run_in_threadpool(handler, request, exc)
                await response(scope, receive, send)
                return

            # Walk MRO for exception class handler
            handler = self._lookup_handler(exc)
            if handler is not None:
                if is_async_callable(handler):
                    response = await handler(request, exc)
                else:
                    response = await run_in_threadpool(handler, request, exc)
                await response(scope, receive, send)
                return

            # No handler found -- re-raise
            raise


# ---------------------------------------------------------------------------
# BaseHTTPMiddleware
# ---------------------------------------------------------------------------


class BaseHTTPMiddleware:
    """Base class for user-defined HTTP middleware using the high-level
    dispatch pattern.

    Subclasses override ``dispatch(request, call_next) -> Response``.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    dispatch : callable or None
        Optional dispatch callable (overrides the ``dispatch`` method).
    """

    def __init__(
        self,
        app: ASGIApp,
        dispatch: Optional[Callable[..., Any]] = None,
    ) -> None:
        self.app = app
        if dispatch is not None:
            self.dispatch = dispatch  # type: ignore[assignment]

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        request = Request(scope, receive, send)

        async def call_next(request: Request) -> Response:
            """Invoke the inner app and capture its response."""
            app_exc: Optional[Exception] = None
            status_code = 500
            headers_raw: list = []
            body_parts: list[bytes] = []
            body_complete = asyncio.Event()

            async def receive_wrapper() -> Any:
                return await request.receive()

            async def send_wrapper(message: Any) -> None:
                nonlocal status_code, headers_raw
                if message["type"] == "http.response.start":
                    status_code = message["status"]
                    headers_raw = message.get("headers", [])
                elif message["type"] == "http.response.body":
                    body_chunk = message.get("body", b"")
                    if body_chunk:
                        body_parts.append(body_chunk)
                    if not message.get("more_body", False):
                        body_complete.set()

            try:
                await self.app(scope, receive_wrapper, send_wrapper)
            except Exception as exc:
                app_exc = exc
                body_complete.set()

            await body_complete.wait()

            if app_exc is not None:
                raise app_exc

            body = b"".join(body_parts)
            response = Response(
                content=body,
                status_code=status_code,
            )
            response._raw_headers = list(headers_raw)
            return response

        response = await self.dispatch(request, call_next)
        await response(scope, receive, send)

    async def dispatch(self, request: Request, call_next: Callable[..., Any]) -> Response:
        """Override this method to implement middleware logic.

        Parameters
        ----------
        request : Request
            The incoming HTTP request.
        call_next : callable
            Calls the next middleware / route handler and returns a Response.
        """
        raise NotImplementedError("Subclasses must implement dispatch()")


# ---------------------------------------------------------------------------
# CORSMiddleware
# ---------------------------------------------------------------------------


class CORSMiddleware:
    """Full CORS (Cross-Origin Resource Sharing) middleware.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    allow_origins : sequence of str
        Origins that are permitted.  Use ``["*"]`` to allow all.
    allow_origin_regex : str or None
        Regex pattern for allowed origins.
    allow_methods : sequence of str
        HTTP methods to allow.  ``["*"]`` means all standard methods.
    allow_headers : sequence of str
        HTTP headers to allow.  ``["*"]`` means all headers.
    allow_credentials : bool
        Whether to allow credentials (cookies, auth headers).
    expose_headers : sequence of str
        Headers to expose to the browser.
    max_age : int
        Max age in seconds for preflight cache.
    """

    # Standard "simple" headers that are always exposed
    _SIMPLE_HEADERS: set = {
        "accept",
        "accept-language",
        "content-language",
        "content-type",
    }

    _ALL_METHODS: list = [
        "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT",
    ]

    def __init__(
        self,
        app: ASGIApp,
        allow_origins: Sequence[str] = (),
        allow_origin_regex: Optional[str] = None,
        allow_methods: Sequence[str] = ("GET",),
        allow_headers: Sequence[str] = (),
        allow_credentials: bool = False,
        expose_headers: Sequence[str] = (),
        max_age: int = 600,
    ) -> None:
        self.app = app

        # Origins
        self.allow_all_origins = "*" in allow_origins
        self.allow_origins = [o.lower() for o in allow_origins]
        self.allow_origin_regex: Optional[re.Pattern] = None
        if allow_origin_regex is not None:
            self.allow_origin_regex = re.compile(allow_origin_regex)

        # Methods
        if "*" in allow_methods:
            self.allow_methods = self._ALL_METHODS
        else:
            self.allow_methods = [m.upper() for m in allow_methods]

        # Headers
        self.allow_all_headers = "*" in allow_headers
        self.allow_headers = [h.lower() for h in allow_headers]

        self.allow_credentials = allow_credentials
        self.expose_headers = list(expose_headers)
        self.max_age = max_age

        # Pre-compute simple response headers that never change
        self.simple_headers: list[tuple[bytes, bytes]] = []
        if self.allow_all_origins:
            self.simple_headers.append((b"access-control-allow-origin", b"*"))
        if self.expose_headers:
            self.simple_headers.append(
                (
                    b"access-control-expose-headers",
                    ", ".join(self.expose_headers).encode("latin-1"),
                )
            )
        if self.allow_credentials:
            self.simple_headers.append(
                (b"access-control-allow-credentials", b"true")
            )

        # Pre-compute preflight response headers
        self.preflight_headers: list[tuple[bytes, bytes]] = []
        if self.allow_all_origins:
            self.preflight_headers.append(
                (b"access-control-allow-origin", b"*")
            )
        if self.allow_credentials:
            self.preflight_headers.append(
                (b"access-control-allow-credentials", b"true")
            )
        self.preflight_headers.append(
            (
                b"access-control-allow-methods",
                ", ".join(self.allow_methods).encode("latin-1"),
            )
        )
        self.preflight_headers.append(
            (b"access-control-max-age", str(self.max_age).encode("latin-1"))
        )

    def _is_origin_allowed(self, origin: str) -> bool:
        if self.allow_all_origins:
            return True
        origin_lower = origin.lower()
        if origin_lower in self.allow_origins:
            return True
        if self.allow_origin_regex is not None and self.allow_origin_regex.fullmatch(origin):
            return True
        return False

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        headers = dict(
            (k.decode("latin-1") if isinstance(k, bytes) else k,
             v.decode("latin-1") if isinstance(v, bytes) else v)
            for k, v in scope.get("headers", [])
        )
        origin = headers.get("origin")

        if origin is None:
            # Not a CORS request
            await self.app(scope, receive, send)
            return

        method = scope.get("method", "GET")
        if method == "OPTIONS" and "access-control-request-method" in headers:
            # Preflight request
            response = self._preflight_response(origin, headers)
            await response(scope, receive, send)
            return

        # Simple / actual request -- run app then inject headers
        await self._simple_response(scope, receive, send, origin)

    def _preflight_response(
        self, origin: str, headers: Dict[str, str]
    ) -> Response:
        """Build a preflight response."""
        if not self._is_origin_allowed(origin):
            # Deny: return 400 with no CORS headers
            return PlainTextResponse(
                "Disallowed CORS origin", status_code=400
            )

        response_headers: list[tuple[bytes, bytes]] = list(self.preflight_headers)

        # If not allow_all_origins, echo the specific origin
        if not self.allow_all_origins:
            response_headers.append(
                (b"access-control-allow-origin", origin.encode("latin-1"))
            )
            response_headers.append((b"vary", b"Origin"))

        # Allowed headers
        requested_headers = headers.get("access-control-request-headers", "")
        if self.allow_all_headers and requested_headers:
            response_headers.append(
                (
                    b"access-control-allow-headers",
                    requested_headers.encode("latin-1"),
                )
            )
        elif self.allow_headers:
            response_headers.append(
                (
                    b"access-control-allow-headers",
                    ", ".join(self.allow_headers).encode("latin-1"),
                )
            )

        resp = Response(status_code=200)
        resp._raw_headers = response_headers
        resp.body = b""
        return resp

    async def _simple_response(
        self, scope: Scope, receive: Receive, send: Send, origin: str
    ) -> None:
        """Run the app and inject CORS headers into the response."""
        headers_to_add: list[tuple[bytes, bytes]] = list(self.simple_headers)

        if not self.allow_all_origins:
            if self._is_origin_allowed(origin):
                headers_to_add.append(
                    (b"access-control-allow-origin", origin.encode("latin-1"))
                )
                headers_to_add.append((b"vary", b"Origin"))
            else:
                # Origin not allowed -- run app without CORS headers
                await self.app(scope, receive, send)
                return

        async def send_with_cors(message: Any) -> None:
            if message["type"] == "http.response.start":
                existing = list(message.get("headers", []))
                existing.extend(headers_to_add)
                message = dict(message)
                message["headers"] = existing
            await send(message)

        await self.app(scope, receive, send_with_cors)


# ---------------------------------------------------------------------------
# GZipMiddleware
# ---------------------------------------------------------------------------


class GZipMiddleware:
    """ASGI middleware that gzip-compresses response bodies larger than
    the configured minimum size.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    minimum_size : int
        Responses smaller than this (in bytes) are not compressed.
    compresslevel : int
        Gzip compression level (1-9, default 9).
    """

    def __init__(
        self,
        app: ASGIApp,
        minimum_size: int = 500,
        compresslevel: int = 9,
    ) -> None:
        self.app = app
        self.minimum_size = minimum_size
        self.compresslevel = compresslevel

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        # Check if client accepts gzip
        headers = dict(
            (k.decode("latin-1") if isinstance(k, bytes) else k,
             v.decode("latin-1") if isinstance(v, bytes) else v)
            for k, v in scope.get("headers", [])
        )
        accept_encoding = headers.get("accept-encoding", "")
        if "gzip" not in accept_encoding:
            await self.app(scope, receive, send)
            return

        responder = _GZipResponder(
            self.app, self.minimum_size, self.compresslevel
        )
        await responder(scope, receive, send)


class _GZipResponder:
    """Internal helper that intercepts response messages for gzip
    compression."""

    def __init__(self, app: ASGIApp, minimum_size: int, compresslevel: int) -> None:
        self.app = app
        self.minimum_size = minimum_size
        self.compresslevel = compresslevel
        self.initial_message: Optional[dict] = None
        self.body_parts: list[bytes] = []
        self.started = False

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        self._send = send
        await self.app(scope, receive, self.send_wrapper)

        # If we buffered everything (streaming not started), flush now
        if not self.started and self.initial_message is not None:
            await self._flush_buffered()

    async def send_wrapper(self, message: Any) -> None:
        if message["type"] == "http.response.start":
            self.initial_message = message
            return

        if message["type"] == "http.response.body":
            body = message.get("body", b"")
            more_body = message.get("more_body", False)

            if not self.started:
                self.body_parts.append(body)
                if more_body:
                    # Keep buffering -- we might need to check total size
                    return
                # End of body: flush everything at once
                return
            else:
                # Already streaming (decided not to compress or already sent)
                await self._send(message)

    async def _flush_buffered(self) -> None:
        """Compress (if large enough) and send the buffered response."""
        full_body = b"".join(self.body_parts)
        self.started = True

        if len(full_body) < self.minimum_size:
            # Too small -- send uncompressed
            await self._send(self.initial_message)
            await self._send(
                {"type": "http.response.body", "body": full_body}
            )
            return

        # Check content-type -- skip compression for already-compressed types
        resp_headers = list(self.initial_message.get("headers", []))
        content_type = ""
        for name, value in resp_headers:
            n = name.decode("latin-1") if isinstance(name, bytes) else name
            if n.lower() == "content-type":
                content_type = (
                    value.decode("latin-1") if isinstance(value, bytes) else value
                )
                break

        skip_types = (
            "image/", "audio/", "video/",
            "application/zip", "application/gzip",
            "application/x-gzip", "application/octet-stream",
        )
        if any(content_type.startswith(t) for t in skip_types):
            await self._send(self.initial_message)
            await self._send(
                {"type": "http.response.body", "body": full_body}
            )
            return

        # Compress with gzip
        compressed = gzip.compress(full_body, compresslevel=self.compresslevel)

        # Update headers: content-encoding, content-length, remove old
        # content-length, add vary
        new_headers: list[tuple[bytes, bytes]] = []
        for name, value in resp_headers:
            n = name.decode("latin-1") if isinstance(name, bytes) else name
            if n.lower() == "content-length":
                continue  # Will replace
            new_headers.append((name, value))

        new_headers.append((b"content-encoding", b"gzip"))
        new_headers.append(
            (b"content-length", str(len(compressed)).encode("latin-1"))
        )
        new_headers.append((b"vary", b"Accept-Encoding"))

        start_message = dict(self.initial_message)
        start_message["headers"] = new_headers
        await self._send(start_message)
        await self._send(
            {"type": "http.response.body", "body": compressed}
        )


# ---------------------------------------------------------------------------
# TrustedHostMiddleware
# ---------------------------------------------------------------------------


class TrustedHostMiddleware:
    """ASGI middleware that validates the Host header against an allow list.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    allowed_hosts : sequence of str or None
        List of allowed host patterns.  Entries starting with ``*.`` match
        any subdomain.  Use ``["*"]`` or ``None`` to allow everything.
    """

    def __init__(
        self,
        app: ASGIApp,
        allowed_hosts: Optional[Sequence[str]] = None,
    ) -> None:
        self.app = app
        if allowed_hosts is None or "*" in (allowed_hosts or []):
            self.allowed_hosts: list[str] = ["*"]
            self.allow_any = True
        else:
            self.allowed_hosts = [h.lower() for h in allowed_hosts]
            self.allow_any = False

    def _is_host_allowed(self, host: str) -> bool:
        if self.allow_any:
            return True
        # Strip port if present
        host_without_port = host.split(":")[0].lower()
        for pattern in self.allowed_hosts:
            if pattern == host_without_port:
                return True
            # Wildcard subdomain: *.example.com
            if pattern.startswith("*."):
                suffix = pattern[1:]  # .example.com
                if host_without_port.endswith(suffix):
                    return True
                # Also allow the bare domain (e.g., example.com for *.example.com)
                if host_without_port == pattern[2:]:
                    return True
        return False

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if self.allow_any or scope["type"] not in ("http", "websocket"):
            await self.app(scope, receive, send)
            return

        # Extract host header
        headers = dict(
            (k.decode("latin-1") if isinstance(k, bytes) else k,
             v.decode("latin-1") if isinstance(v, bytes) else v)
            for k, v in scope.get("headers", [])
        )
        host = headers.get("host", "")

        if not self._is_host_allowed(host):
            response = PlainTextResponse("Invalid host header", status_code=400)
            await response(scope, receive, send)
            return

        await self.app(scope, receive, send)


# ---------------------------------------------------------------------------
# HTTPSRedirectMiddleware
# ---------------------------------------------------------------------------


class HTTPSRedirectMiddleware:
    """ASGI middleware that redirects HTTP requests to HTTPS.

    Parameters
    ----------
    app : ASGIApp
        The inner application.
    """

    def __init__(self, app: ASGIApp) -> None:
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] not in ("http", "websocket"):
            await self.app(scope, receive, send)
            return

        scheme = scope.get("scheme", "")
        if scheme in ("https", "wss"):
            await self.app(scope, receive, send)
            return

        # Build the redirect URL
        if scope["type"] == "websocket":
            redirect_scheme = "wss"
        else:
            redirect_scheme = "https"

        server = scope.get("server")
        path = scope.get("root_path", "") + scope.get("path", "/")
        query_string = scope.get("query_string", b"")

        if server:
            host_str, port = server
            default_secure_ports = {"https": 443, "wss": 443}
            if port == default_secure_ports.get(redirect_scheme):
                netloc = host_str
            else:
                netloc = f"{host_str}:{port}"
            url = f"{redirect_scheme}://{netloc}{path}"
        else:
            # Fallback: try host header
            headers = dict(
                (k.decode("latin-1") if isinstance(k, bytes) else k,
                 v.decode("latin-1") if isinstance(v, bytes) else v)
                for k, v in scope.get("headers", [])
            )
            host = headers.get("host", "localhost")
            url = f"{redirect_scheme}://{host}{path}"

        if query_string:
            qs = (
                query_string.decode("latin-1")
                if isinstance(query_string, bytes)
                else query_string
            )
            url = f"{url}?{qs}"

        response = RedirectResponse(url, status_code=307)
        await response(scope, receive, send)


# ---------------------------------------------------------------------------
# WSGIMiddleware
# ---------------------------------------------------------------------------


class WSGIMiddleware:
    """ASGI middleware that bridges a WSGI application to ASGI.

    Parameters
    ----------
    app : WSGI callable
        A PEP-3333 WSGI application ``(environ, start_response)``.
    workers : int
        Number of thread pool workers for running the WSGI app.
    """

    def __init__(self, app: Any, workers: int = 10) -> None:
        self.app = app
        self.executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=workers
        )

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            raise TypeError(
                f"WSGIMiddleware does not support scope type {scope['type']!r}"
            )

        # Read the full request body
        body_parts: list[bytes] = []
        while True:
            message = await receive()
            body_chunk = message.get("body", b"")
            if body_chunk:
                body_parts.append(body_chunk)
            if not message.get("more_body", False):
                break
        body = b"".join(body_parts)

        # Build WSGI environ
        environ = _build_wsgi_environ(scope, body)

        # Run the WSGI app in a thread
        loop = asyncio.get_running_loop()
        status_code, response_headers, response_body = await loop.run_in_executor(
            self.executor, self._run_wsgi, environ
        )

        # Send the response
        raw_headers: list[tuple[bytes, bytes]] = [
            (name.lower().encode("latin-1"), value.encode("latin-1"))
            for name, value in response_headers
        ]
        await send(
            {
                "type": "http.response.start",
                "status": status_code,
                "headers": raw_headers,
            }
        )
        await send({"type": "http.response.body", "body": response_body})

    def _run_wsgi(self, environ: dict) -> Tuple[int, list, bytes]:
        """Execute the WSGI app synchronously and collect the response."""
        status_code = 500
        response_headers: list[tuple[str, str]] = []
        body_parts: list[bytes] = []

        def start_response(
            status: str,
            headers: list,
            exc_info: Any = None,
        ) -> Callable:
            nonlocal status_code, response_headers
            status_code = int(status.split(" ", 1)[0])
            response_headers = list(headers)
            return lambda s: body_parts.append(s)

        try:
            result = self.app(environ, start_response)
            try:
                for chunk in result:
                    if chunk:
                        body_parts.append(chunk)
            finally:
                if hasattr(result, "close"):
                    result.close()
        except Exception:
            status_code = 500
            response_headers = [("Content-Type", "text/plain")]
            body_parts = [b"Internal Server Error"]

        return status_code, response_headers, b"".join(body_parts)


def _build_wsgi_environ(scope: Scope, body: bytes) -> dict:
    """Convert an ASGI HTTP scope into a WSGI environ dict."""
    server = scope.get("server") or ("localhost", 80)
    host, port = server

    # Extract headers into environ
    headers_raw = scope.get("headers", [])
    environ: dict[str, Any] = {
        "REQUEST_METHOD": scope.get("method", "GET"),
        "SCRIPT_NAME": scope.get("root_path", ""),
        "PATH_INFO": scope.get("path", "/"),
        "QUERY_STRING": (
            scope.get("query_string", b"").decode("latin-1")
            if isinstance(scope.get("query_string", b""), bytes)
            else scope.get("query_string", "")
        ),
        "SERVER_NAME": host,
        "SERVER_PORT": str(port),
        "SERVER_PROTOCOL": f"HTTP/{scope.get('http_version', '1.1')}",
        "wsgi.version": (1, 0),
        "wsgi.url_scheme": scope.get("scheme", "http"),
        "wsgi.input": io.BytesIO(body),
        "wsgi.errors": sys.stderr,
        "wsgi.multithread": True,
        "wsgi.multiprocess": True,
        "wsgi.run_once": False,
    }

    for name_bytes, value_bytes in headers_raw:
        name = (
            name_bytes.decode("latin-1")
            if isinstance(name_bytes, bytes)
            else name_bytes
        )
        value = (
            value_bytes.decode("latin-1")
            if isinstance(value_bytes, bytes)
            else value_bytes
        )
        name_upper = name.upper().replace("-", "_")
        if name_upper == "CONTENT_TYPE":
            environ["CONTENT_TYPE"] = value
        elif name_upper == "CONTENT_LENGTH":
            environ["CONTENT_LENGTH"] = value
        else:
            key = f"HTTP_{name_upper}"
            existing = environ.get(key)
            if existing:
                environ[key] = f"{existing},{value}"
            else:
                environ[key] = value

    # Client info
    client = scope.get("client")
    if client:
        environ["REMOTE_ADDR"] = client[0]
        environ["REMOTE_PORT"] = str(client[1])

    return environ
