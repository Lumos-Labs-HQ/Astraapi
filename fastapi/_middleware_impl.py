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
import io
import re
import sys
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
from fastapi._request import Request
from fastapi._response import (
    Response,
    PlainTextResponse,
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
            self._exact_hosts: set[str] = set()
            self._wildcard_suffixes: list[str] = []
            self._wildcard_bare_domains: set[str] = set()
        else:
            self.allowed_hosts = [h.lower() for h in allowed_hosts]
            self.allow_any = False
            # Pre-build O(1) lookup structures
            self._exact_hosts = set()
            self._wildcard_suffixes = []
            self._wildcard_bare_domains = set()
            for h in self.allowed_hosts:
                if h.startswith("*."):
                    self._wildcard_suffixes.append(h[1:])  # ".example.com"
                    self._wildcard_bare_domains.add(h[2:])  # "example.com"
                else:
                    self._exact_hosts.add(h)

    def _is_host_allowed(self, host: str) -> bool:
        if self.allow_any:
            return True
        # Strip port if present
        host_without_port = host.split(":")[0].lower()
        # O(1) exact match
        if host_without_port in self._exact_hosts:
            return True
        # O(1) bare domain check for wildcard entries
        if host_without_port in self._wildcard_bare_domains:
            return True
        # O(w) wildcard suffix check (w is typically 0-2)
        for suffix in self._wildcard_suffixes:
            if host_without_port.endswith(suffix):
                return True
        return False


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
