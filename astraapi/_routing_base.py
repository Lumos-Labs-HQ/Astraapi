"""
Native routing classes -- replaces starlette.routing.

Provides Match, compile_path, get_name, BaseRoute, Route, WebSocketRoute,
Router, and Mount with zero starlette imports.  The C++ trie router handles
actual route matching in production; this Python regex matching is a fallback.
"""
from __future__ import annotations

import contextlib
import enum
import functools
import inspect
import math
import re
import traceback
import types
import uuid
import warnings
from collections.abc import Awaitable, Callable, Collection, Generator, Sequence
from contextlib import AbstractAsyncContextManager, AbstractContextManager, asynccontextmanager
from re import Pattern
from typing import Any, ClassVar, Generic, Optional, TypeVar

from astraapi._concurrency import is_async_callable, run_in_threadpool
from astraapi._request import Request, URL
from astraapi._response import PlainTextResponse, RedirectResponse, Response
from astraapi._types import ASGIApp, Lifespan, Receive, Scope, Send
from astraapi._websocket import WebSocket


# ---------------------------------------------------------------------------
# NoMatchFound exception
# ---------------------------------------------------------------------------


class NoMatchFound(Exception):
    """
    Raised by `.url_path_for(name, **path_params)` if no matching route exists.
    """

    def __init__(self, name: str, path_params: dict[str, Any]) -> None:
        params = ", ".join(list(path_params.keys()))
        super().__init__(f'No route exists for name "{name}" and params "{params}".')


# ---------------------------------------------------------------------------
# Match enum
# ---------------------------------------------------------------------------


class Match(enum.Enum):
    NONE = 0
    PARTIAL = 1
    FULL = 2


# ---------------------------------------------------------------------------
# Convertors
# ---------------------------------------------------------------------------

T = TypeVar("T")


class Convertor(Generic[T]):
    """Base convertor class for path parameters."""

    regex: ClassVar[str] = ""

    def convert(self, value: str) -> T:
        raise NotImplementedError()

    def to_string(self, value: T) -> str:
        raise NotImplementedError()


class StringConvertor(Convertor[str]):
    regex = "[^/]+"

    def convert(self, value: str) -> str:
        return value

    def to_string(self, value: str) -> str:
        value = str(value)
        assert "/" not in value, "May not contain path separators"
        assert value, "Must not be empty"
        return value


class IntegerConvertor(Convertor[int]):
    regex = "[0-9]+"

    def convert(self, value: str) -> int:
        return int(value)

    def to_string(self, value: int) -> str:
        value = int(value)
        assert value >= 0, "Negative integers are not supported"
        return str(value)


class FloatConvertor(Convertor[float]):
    regex = r"[0-9]+(\.[0-9]+)?"

    def convert(self, value: str) -> float:
        return float(value)

    def to_string(self, value: float) -> str:
        value = float(value)
        assert value >= 0.0, "Negative floats are not supported"
        assert not math.isnan(value), "NaN values are not supported"
        assert not math.isinf(value), "Infinite values are not supported"
        return ("%0.20f" % value).rstrip("0").rstrip(".")


class PathConvertor(Convertor[str]):
    regex = ".*"

    def convert(self, value: str) -> str:
        return str(value)

    def to_string(self, value: str) -> str:
        return str(value)


class UUIDConvertor(Convertor[uuid.UUID]):
    regex = "[0-9a-fA-F]{8}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{4}-?[0-9a-fA-F]{12}"

    def convert(self, value: str) -> uuid.UUID:
        return uuid.UUID(value)

    def to_string(self, value: uuid.UUID) -> str:
        return str(value)


CONVERTOR_TYPES: dict[str, Convertor[Any]] = {
    "str": StringConvertor(),
    "path": PathConvertor(),
    "int": IntegerConvertor(),
    "float": FloatConvertor(),
    "uuid": UUIDConvertor(),
}


def register_url_convertor(key: str, convertor: Convertor[Any]) -> None:
    CONVERTOR_TYPES[key] = convertor


# ---------------------------------------------------------------------------
# URLPath -- a str subclass that carries protocol/host metadata
# ---------------------------------------------------------------------------


class URLPath(str):
    """
    A URL path string that may also hold an associated protocol and/or host.
    Used by the routing to return ``url_path_for`` matches.
    """

    def __new__(cls, path: str, protocol: str = "", host: str = "") -> URLPath:
        assert protocol in ("http", "websocket", "")
        return str.__new__(cls, path)

    def __init__(self, path: str, protocol: str = "", host: str = "") -> None:
        self.protocol = protocol
        self.host = host

    def make_absolute_url(self, base_url: str | URL) -> URL:
        if isinstance(base_url, str):
            base_url = URL(base_url)
        if self.protocol:
            scheme = {
                "http": {True: "https", False: "http"},
                "websocket": {True: "wss", False: "ws"},
            }[self.protocol][base_url.is_secure]
        else:
            scheme = base_url.scheme

        netloc = self.host or base_url.netloc
        path = base_url.path.rstrip("/") + str(self)
        return URL(f"{scheme}://{netloc}{path}")


# ---------------------------------------------------------------------------
# WebSocketClose -- lightweight ASGI close
# ---------------------------------------------------------------------------


class WebSocketClose:
    """Send a WebSocket close frame."""

    def __init__(self, code: int = 1000, reason: Optional[str] = None) -> None:
        self.code = code
        self.reason = reason or ""

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        await send({"type": "websocket.close", "code": self.code, "reason": self.reason})


# ---------------------------------------------------------------------------
# Helper: get_route_path
# ---------------------------------------------------------------------------


def get_route_path(scope: Scope) -> str:
    """Extract the route-matching path from the scope, stripping root_path."""
    path: str = scope["path"]
    root_path = scope.get("root_path", "")
    if not root_path:
        return path
    if not path.startswith(root_path):
        return path
    if path == root_path:
        return ""
    if path[len(root_path)] == "/":
        return path[len(root_path):]
    return path


# ---------------------------------------------------------------------------
# Helper: get_name
# ---------------------------------------------------------------------------


def get_name(endpoint: Callable[..., Any]) -> str:
    """Return the name of an endpoint function or class."""
    return getattr(endpoint, "__name__", endpoint.__class__.__name__)


# ---------------------------------------------------------------------------
# Helper: replace_params
# ---------------------------------------------------------------------------


def replace_params(
    path: str,
    param_convertors: dict[str, Convertor[Any]],
    path_params: dict[str, str],
) -> tuple[str, dict[str, str]]:
    """Replace path parameters in a path format string with their values."""
    for key, value in list(path_params.items()):
        if "{" + key + "}" in path:
            convertor = param_convertors[key]
            value = convertor.to_string(value)
            path = path.replace("{" + key + "}", value)
            path_params.pop(key)
    return path, path_params


# ---------------------------------------------------------------------------
# compile_path
# ---------------------------------------------------------------------------

# Match parameters in URL paths, eg. '{param}', and '{param:int}'
PARAM_REGEX = re.compile("{([a-zA-Z_][a-zA-Z0-9_]*)(:[a-zA-Z_][a-zA-Z0-9_]*)?}")


def compile_path(
    path: str,
) -> tuple[Pattern[str], str, dict[str, Convertor[Any]]]:
    """
    Given a path string, like: "/{username:str}",
    or a host string, like: "{subdomain}.mydomain.org", return a three-tuple
    of (regex, format, {param_name:convertor}).

    regex:      "/(?P<username>[^/]+)"
    format:     "/{username}"
    convertors: {"username": StringConvertor()}
    """
    is_host = not path.startswith("/")

    path_regex = "^"
    path_format = ""
    duplicated_params: set[str] = set()

    idx = 0
    param_convertors: dict[str, Convertor[Any]] = {}
    for match in PARAM_REGEX.finditer(path):
        param_name, convertor_type = match.groups("str")
        convertor_type = convertor_type.lstrip(":")
        assert convertor_type in CONVERTOR_TYPES, f"Unknown path convertor '{convertor_type}'"
        convertor = CONVERTOR_TYPES[convertor_type]

        path_regex += re.escape(path[idx:match.start()])
        path_regex += f"(?P<{param_name}>{convertor.regex})"

        path_format += path[idx:match.start()]
        path_format += "{%s}" % param_name

        if param_name in param_convertors:
            duplicated_params.add(param_name)

        param_convertors[param_name] = convertor

        idx = match.end()

    if duplicated_params:
        names = ", ".join(sorted(duplicated_params))
        ending = "s" if len(duplicated_params) > 1 else ""
        raise ValueError(f"Duplicated param name{ending} {names} at path {path}")

    if is_host:
        # Align with Host.matches() behavior, which ignores port.
        hostname = path[idx:].split(":")[0]
        path_regex += re.escape(hostname) + "$"
    else:
        path_regex += re.escape(path[idx:]) + "$"

    path_format += path[idx:]

    return re.compile(path_regex), path_format, param_convertors


# ---------------------------------------------------------------------------
# Middleware type placeholder
# ---------------------------------------------------------------------------


class Middleware:
    """Lightweight middleware descriptor (cls, args, kwargs)."""

    def __init__(self, cls: Any, *args: Any, **kwargs: Any) -> None:
        self.cls = cls
        self.args = args
        self.kwargs = kwargs

    def __iter__(self):
        return iter((self.cls, self.args, self.kwargs))

    def __repr__(self) -> str:
        class_name = self.__class__.__name__
        args_strings = [f"{value!r}" for value in self.args]
        option_strings = [f"{key}={value!r}" for key, value in self.kwargs.items()]
        name = getattr(self.cls, "__name__", "")
        args_repr = ", ".join([name] + args_strings + option_strings)
        return f"{class_name}({args_repr})"


# ---------------------------------------------------------------------------
# request_response / websocket_session -- ASGI adapters
# ---------------------------------------------------------------------------


def request_response(
    func: Callable[[Request], Awaitable[Response] | Response],
) -> ASGIApp:
    """
    Takes a function or coroutine ``func(request) -> response``,
    and returns an ASGI application.
    """
    from astraapi._exception_handler import wrap_app_handling_exceptions

    f: Callable[[Request], Awaitable[Response]] = (
        func if is_async_callable(func) else functools.partial(run_in_threadpool, func)
    )

    async def app(scope: Scope, receive: Receive, send: Send) -> None:
        request = Request(scope, receive, send)

        async def app(scope: Scope, receive: Receive, send: Send) -> None:
            response = await f(request)
            await response(scope, receive, send)

        await wrap_app_handling_exceptions(app, request)(scope, receive, send)

    return app


def websocket_session(
    func: Callable[[WebSocket], Awaitable[None]],
) -> ASGIApp:
    """
    Takes a coroutine ``func(session)``, and returns an ASGI application.
    """
    from astraapi._exception_handler import wrap_app_handling_exceptions

    async def app(scope: Scope, receive: Receive, send: Send) -> None:
        session = WebSocket(scope, receive=receive, send=send)

        async def app(scope: Scope, receive: Receive, send: Send) -> None:
            await func(session)

        await wrap_app_handling_exceptions(app, session)(scope, receive, send)

    return app


# ---------------------------------------------------------------------------
# BaseRoute
# ---------------------------------------------------------------------------


class BaseRoute:
    """Abstract base class for all route types."""

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        raise NotImplementedError()

    def url_path_for(self, name: str, /, **path_params: Any) -> URLPath:
        raise NotImplementedError()

    async def handle(self, scope: Scope, receive: Receive, send: Send) -> None:
        raise NotImplementedError()

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        """
        A route may be used in isolation as a stand-alone ASGI app.
        This is a somewhat contrived case, as they'll almost always be used
        within a Router, but could be useful for some tooling and minimal apps.
        """
        match, child_scope = self.matches(scope)
        if match == Match.NONE:
            if scope["type"] == "http":
                response = PlainTextResponse("Not Found", status_code=404)
                await response(scope, receive, send)
            elif scope["type"] == "websocket":
                websocket_close = WebSocketClose()
                await websocket_close(scope, receive, send)
            return

        scope.update(child_scope)
        await self.handle(scope, receive, send)


# ---------------------------------------------------------------------------
# Route
# ---------------------------------------------------------------------------


class Route(BaseRoute):
    """HTTP route that matches against a path regex and dispatches to an endpoint."""

    def __init__(
        self,
        path: str,
        endpoint: Callable[..., Any],
        *,
        methods: Collection[str] | None = None,
        name: str | None = None,
        include_in_schema: bool = True,
        middleware: Sequence[Middleware] | None = None,
    ) -> None:
        assert path.startswith("/"), "Routed paths must start with '/'"
        self.path = path
        self.endpoint = endpoint
        self.name = get_name(endpoint) if name is None else name
        self.include_in_schema = include_in_schema

        endpoint_handler = endpoint
        while isinstance(endpoint_handler, functools.partial):
            endpoint_handler = endpoint_handler.func
        if inspect.isfunction(endpoint_handler) or inspect.ismethod(endpoint_handler):
            # Endpoint is function or method. Treat it as `func(request) -> response`.
            self.app = request_response(endpoint)
            if methods is None:
                methods = ["GET"]
        else:
            # Endpoint is a class. Treat it as ASGI.
            self.app = endpoint

        if middleware is not None:
            for cls, args, kwargs in reversed(middleware):
                self.app = cls(self.app, *args, **kwargs)

        if methods is None:
            self.methods: set[str] | None = None
        else:
            self.methods = {method.upper() for method in methods}
            if "GET" in self.methods:
                self.methods.add("HEAD")

        self.path_regex, self.path_format, self.param_convertors = compile_path(path)

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        path_params: dict[str, Any]
        if scope["type"] == "http":
            route_path = get_route_path(scope)
            match = self.path_regex.match(route_path)
            if match:
                matched_params = match.groupdict()
                for key, value in matched_params.items():
                    matched_params[key] = self.param_convertors[key].convert(value)
                path_params = dict(scope.get("path_params", {}))
                path_params.update(matched_params)
                child_scope = {"endpoint": self.endpoint, "path_params": path_params}
                if self.methods and scope["method"] not in self.methods:
                    return Match.PARTIAL, child_scope
                else:
                    return Match.FULL, child_scope
        return Match.NONE, {}

    def url_path_for(self, name: str, /, **path_params: Any) -> URLPath:
        seen_params = set(path_params.keys())
        expected_params = set(self.param_convertors.keys())

        if name != self.name or seen_params != expected_params:
            raise NoMatchFound(name, path_params)

        path, remaining_params = replace_params(self.path_format, self.param_convertors, path_params)
        assert not remaining_params
        return URLPath(path=path, protocol="http")

    async def handle(self, scope: Scope, receive: Receive, send: Send) -> None:
        if self.methods and scope["method"] not in self.methods:
            headers = {"Allow": ", ".join(self.methods)}
            if "app" in scope:
                from astraapi.exceptions import HTTPException
                raise HTTPException(status_code=405, headers=headers)
            else:
                response = PlainTextResponse(
                    "Method Not Allowed", status_code=405, headers=headers
                )
            await response(scope, receive, send)
        else:
            await self.app(scope, receive, send)

    def __eq__(self, other: Any) -> bool:
        return (
            isinstance(other, Route)
            and self.path == other.path
            and self.endpoint == other.endpoint
            and self.methods == other.methods
        )

    def __repr__(self) -> str:
        class_name = self.__class__.__name__
        methods = sorted(self.methods or [])
        path, name = self.path, self.name
        return f"{class_name}(path={path!r}, name={name!r}, methods={methods!r})"


# ---------------------------------------------------------------------------
# WebSocketRoute
# ---------------------------------------------------------------------------


class WebSocketRoute(BaseRoute):
    """WebSocket route that matches against a path regex and dispatches to an endpoint."""

    def __init__(
        self,
        path: str,
        endpoint: Callable[..., Any],
        *,
        name: str | None = None,
        middleware: Sequence[Middleware] | None = None,
    ) -> None:
        assert path.startswith("/"), "Routed paths must start with '/'"
        self.path = path
        self.endpoint = endpoint
        self.name = get_name(endpoint) if name is None else name

        endpoint_handler = endpoint
        while isinstance(endpoint_handler, functools.partial):
            endpoint_handler = endpoint_handler.func
        if inspect.isfunction(endpoint_handler) or inspect.ismethod(endpoint_handler):
            # Endpoint is function or method. Treat it as `func(websocket)`.
            self.app = websocket_session(endpoint)
        else:
            # Endpoint is a class. Treat it as ASGI.
            self.app = endpoint

        if middleware is not None:
            for cls, args, kwargs in reversed(middleware):
                self.app = cls(self.app, *args, **kwargs)

        self.path_regex, self.path_format, self.param_convertors = compile_path(path)

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        path_params: dict[str, Any]
        if scope["type"] == "websocket":
            route_path = get_route_path(scope)
            match = self.path_regex.match(route_path)
            if match:
                matched_params = match.groupdict()
                for key, value in matched_params.items():
                    matched_params[key] = self.param_convertors[key].convert(value)
                path_params = dict(scope.get("path_params", {}))
                path_params.update(matched_params)
                child_scope = {"endpoint": self.endpoint, "path_params": path_params}
                return Match.FULL, child_scope
        return Match.NONE, {}

    def url_path_for(self, name: str, /, **path_params: Any) -> URLPath:
        seen_params = set(path_params.keys())
        expected_params = set(self.param_convertors.keys())

        if name != self.name or seen_params != expected_params:
            raise NoMatchFound(name, path_params)

        path, remaining_params = replace_params(self.path_format, self.param_convertors, path_params)
        assert not remaining_params
        return URLPath(path=path, protocol="websocket")

    async def handle(self, scope: Scope, receive: Receive, send: Send) -> None:
        await self.app(scope, receive, send)

    def __eq__(self, other: Any) -> bool:
        return (
            isinstance(other, WebSocketRoute)
            and self.path == other.path
            and self.endpoint == other.endpoint
        )

    def __repr__(self) -> str:
        return f"{self.__class__.__name__}(path={self.path!r}, name={self.name!r})"


# ---------------------------------------------------------------------------
# Mount
# ---------------------------------------------------------------------------


class Mount(BaseRoute):
    """Mount a sub-application at a path prefix."""

    def __init__(
        self,
        path: str = "",
        app: ASGIApp | None = None,
        routes: Sequence[BaseRoute] | None = None,
        name: str | None = None,
        *,
        middleware: Sequence[Middleware] | None = None,
    ) -> None:
        assert path == "" or path.startswith("/"), "Routed paths must start with '/'"
        assert app is not None or routes is not None, "Either 'app=...', or 'routes=' must be specified"
        self.path = path.rstrip("/")
        if app is not None:
            self._base_app: ASGIApp = app
        else:
            self._base_app = Router(routes=routes)
        self.app = self._base_app
        if middleware is not None:
            for cls, args, kwargs in reversed(middleware):
                self.app = cls(self.app, *args, **kwargs)
        self.name = name
        self.path_regex, self.path_format, self.param_convertors = compile_path(
            self.path + "/{path:path}"
        )

    @property
    def routes(self) -> list[BaseRoute]:
        return getattr(self._base_app, "routes", [])

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        path_params: dict[str, Any]
        if scope["type"] in ("http", "websocket"):
            root_path = scope.get("root_path", "")
            route_path = get_route_path(scope)
            match = self.path_regex.match(route_path)
            if match:
                matched_params = match.groupdict()
                for key, value in matched_params.items():
                    matched_params[key] = self.param_convertors[key].convert(value)
                remaining_path = "/" + matched_params.pop("path")
                matched_path = route_path[:-len(remaining_path)]
                path_params = dict(scope.get("path_params", {}))
                path_params.update(matched_params)
                child_scope = {
                    "path_params": path_params,
                    "app_root_path": scope.get("app_root_path", root_path),
                    "root_path": root_path + matched_path,
                    "endpoint": self.app,
                }
                return Match.FULL, child_scope
        return Match.NONE, {}

    def url_path_for(self, name: str, /, **path_params: Any) -> URLPath:
        if self.name is not None and name == self.name and "path" in path_params:
            # 'name' matches "<mount_name>".
            path_params["path"] = path_params["path"].lstrip("/")
            path, remaining_params = replace_params(
                self.path_format, self.param_convertors, path_params
            )
            if not remaining_params:
                return URLPath(path=path)
        elif self.name is None or name.startswith(self.name + ":"):
            if self.name is None:
                # No mount name.
                remaining_name = name
            else:
                # 'name' matches "<mount_name>:<child_name>".
                remaining_name = name[len(self.name) + 1:]
            path_kwarg = path_params.get("path")
            path_params["path"] = ""
            path_prefix, remaining_params = replace_params(
                self.path_format, self.param_convertors, path_params
            )
            if path_kwarg is not None:
                remaining_params["path"] = path_kwarg
            for route in self.routes or []:
                try:
                    url = route.url_path_for(remaining_name, **remaining_params)
                    return URLPath(
                        path=path_prefix.rstrip("/") + str(url),
                        protocol=url.protocol,
                    )
                except NoMatchFound:
                    pass
        raise NoMatchFound(name, path_params)

    async def handle(self, scope: Scope, receive: Receive, send: Send) -> None:
        await self.app(scope, receive, send)

    def __eq__(self, other: Any) -> bool:
        return isinstance(other, Mount) and self.path == other.path and self.app == other.app

    def __repr__(self) -> str:
        class_name = self.__class__.__name__
        name = self.name or ""
        return f"{class_name}(path={self.path!r}, name={name!r}, app={self.app!r})"


# ---------------------------------------------------------------------------
# Lifespan helpers (vendored from starlette.routing)
# ---------------------------------------------------------------------------

_T = TypeVar("_T")


class _AsyncLiftContextManager(AbstractAsyncContextManager[_T]):
    def __init__(self, cm: AbstractContextManager[_T]) -> None:
        self._cm = cm

    async def __aenter__(self) -> _T:
        return self._cm.__enter__()

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        tb: types.TracebackType | None,
    ) -> bool | None:
        return self._cm.__exit__(exc_type, exc_value, tb)


def _wrap_gen_lifespan_context(
    lifespan_context: Callable[[Any], Generator[Any, Any, Any]],
) -> Callable[[Any], AbstractAsyncContextManager[Any]]:
    cmgr = contextlib.contextmanager(lifespan_context)

    @functools.wraps(cmgr)
    def wrapper(app: Any) -> _AsyncLiftContextManager[Any]:
        return _AsyncLiftContextManager(cmgr(app))

    return wrapper


class _DefaultLifespan:
    def __init__(self, router: Router) -> None:
        self._router = router

    async def __aenter__(self) -> None:
        await self._router.startup()

    async def __aexit__(self, *exc_info: object) -> None:
        await self._router.shutdown()

    def __call__(self: _T, app: object) -> _T:  # type: ignore[type-var]
        return self


# ---------------------------------------------------------------------------
# Router
# ---------------------------------------------------------------------------


class Router:
    """
    ASGI router that dispatches incoming requests to registered routes.
    """

    def __init__(
        self,
        routes: Sequence[BaseRoute] | None = None,
        redirect_slashes: bool = True,
        default: ASGIApp | None = None,
        on_startup: Sequence[Callable[[], Any]] | None = None,
        on_shutdown: Sequence[Callable[[], Any]] | None = None,
        # the generic to Lifespan[AppType] is the type of the top level application
        # which the router cannot know statically, so we use Any
        lifespan: Lifespan[Any] | None = None,
        *,
        middleware: Sequence[Middleware] | None = None,
    ) -> None:
        self.routes: list[BaseRoute] = [] if routes is None else list(routes)
        self.redirect_slashes = redirect_slashes
        self.default = self.not_found if default is None else default
        self.on_startup: list[Callable[[], Any]] = [] if on_startup is None else list(on_startup)
        self.on_shutdown: list[Callable[[], Any]] = [] if on_shutdown is None else list(on_shutdown)

        if on_startup or on_shutdown:
            warnings.warn(
                "The on_startup and on_shutdown parameters are deprecated, and they "
                "will be removed on version 1.0. Use the lifespan parameter instead. "
                "See more about it on https://starlette.dev/lifespan/.",
                DeprecationWarning,
                stacklevel=2,
            )
            if lifespan:
                warnings.warn(
                    "The `lifespan` parameter cannot be used with `on_startup` or "
                    "`on_shutdown`. Both `on_startup` and `on_shutdown` will be "
                    "ignored.",
                    stacklevel=2,
                )

        if lifespan is None:
            self.lifespan_context: Lifespan[Any] = _DefaultLifespan(self)
        elif inspect.isasyncgenfunction(lifespan):
            warnings.warn(
                "async generator function lifespans are deprecated, "
                "use an @contextlib.asynccontextmanager function instead",
                DeprecationWarning,
                stacklevel=2,
            )
            self.lifespan_context = asynccontextmanager(lifespan)
        elif inspect.isgeneratorfunction(lifespan):
            warnings.warn(
                "generator function lifespans are deprecated, "
                "use an @contextlib.asynccontextmanager function instead",
                DeprecationWarning,
                stacklevel=2,
            )
            self.lifespan_context = _wrap_gen_lifespan_context(lifespan)
        else:
            self.lifespan_context = lifespan

        self.middleware_stack = self.app
        if middleware:
            for cls, args, kwargs in reversed(middleware):
                self.middleware_stack = cls(self.middleware_stack, *args, **kwargs)

    async def not_found(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] == "websocket":
            websocket_close = WebSocketClose()
            await websocket_close(scope, receive, send)
            return

        # If we're running inside an application then raise an exception,
        # so that the configurable exception handler can deal with returning
        # the response.  For plain ASGI apps, just return the response.
        if "app" in scope:
            from astraapi.exceptions import HTTPException
            raise HTTPException(status_code=404)
        else:
            response = PlainTextResponse("Not Found", status_code=404)
        await response(scope, receive, send)

    def url_path_for(self, name: str, /, **path_params: Any) -> URLPath:
        for route in self.routes:
            try:
                return route.url_path_for(name, **path_params)
            except NoMatchFound:
                pass
        raise NoMatchFound(name, path_params)

    async def startup(self) -> None:
        """Run any ``.on_startup`` event handlers."""
        for handler in self.on_startup:
            if is_async_callable(handler):
                await handler()
            else:
                handler()

    async def shutdown(self) -> None:
        """Run any ``.on_shutdown`` event handlers."""
        for handler in self.on_shutdown:
            if is_async_callable(handler):
                await handler()
            else:
                handler()

    async def lifespan(self, scope: Scope, receive: Receive, send: Send) -> None:
        """
        Handle ASGI lifespan messages, which allows us to manage application
        startup and shutdown events.
        """
        started = False
        app: Any = scope.get("app")
        await receive()
        try:
            async with self.lifespan_context(app) as maybe_state:
                if maybe_state is not None:
                    if "state" not in scope:
                        raise RuntimeError(
                            'The server does not support "state" in the lifespan scope.'
                        )
                    scope["state"].update(maybe_state)
                await send({"type": "lifespan.startup.complete"})
                started = True
                await receive()
        except BaseException:
            exc_text = traceback.format_exc()
            if started:
                await send({"type": "lifespan.shutdown.failed", "message": exc_text})
            else:
                await send({"type": "lifespan.startup.failed", "message": exc_text})
            raise
        else:
            await send({"type": "lifespan.shutdown.complete"})

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        """The main entry point to the Router class."""
        await self.middleware_stack(scope, receive, send)

    async def app(self, scope: Scope, receive: Receive, send: Send) -> None:
        assert scope["type"] in ("http", "websocket", "lifespan")

        if "router" not in scope:
            scope["router"] = self

        if scope["type"] == "lifespan":
            await self.lifespan(scope, receive, send)
            return

        partial = None
        partial_scope: dict[str, Any] = {}

        for route in self.routes:
            # Determine if any route matches the incoming scope,
            # and hand over to the matching route if found.
            match, child_scope = route.matches(scope)
            if match == Match.FULL:
                scope.update(child_scope)
                await route.handle(scope, receive, send)
                return
            elif match == Match.PARTIAL and partial is None:
                partial = route
                partial_scope = child_scope

        if partial is not None:
            # Handle partial matches. These are cases where an endpoint is
            # able to handle the request, but is not a preferred option.
            # We use this in particular to deal with "405 Method Not Allowed".
            scope.update(partial_scope)
            await partial.handle(scope, receive, send)
            return

        route_path = get_route_path(scope)
        if scope["type"] == "http" and self.redirect_slashes and route_path != "/":
            redirect_scope = dict(scope)
            if route_path.endswith("/"):
                redirect_scope["path"] = redirect_scope["path"].rstrip("/")
            else:
                redirect_scope["path"] = redirect_scope["path"] + "/"

            for route in self.routes:
                match, child_scope = route.matches(redirect_scope)
                if match != Match.NONE:
                    redirect_url = URL(scope=redirect_scope)
                    response = RedirectResponse(url=str(redirect_url))
                    await response(scope, receive, send)
                    return

        await self.default(scope, receive, send)

    # -- convenience methods (compat with starlette Router) -----------------

    def mount(self, path: str, app: ASGIApp, name: str | None = None) -> None:
        route = Mount(path, app=app, name=name)
        self.routes.append(route)

    def add_route(
        self,
        path: str,
        endpoint: Callable[[Request], Awaitable[Response] | Response],
        methods: Collection[str] | None = None,
        name: str | None = None,
        include_in_schema: bool = True,
    ) -> None:
        route = Route(
            path,
            endpoint=endpoint,
            methods=methods,
            name=name,
            include_in_schema=include_in_schema,
        )
        self.routes.append(route)

    def add_websocket_route(
        self,
        path: str,
        endpoint: Callable[[WebSocket], Awaitable[None]],
        name: str | None = None,
    ) -> None:
        route = WebSocketRoute(path, endpoint=endpoint, name=name)
        self.routes.append(route)
        # Pre-compute WS parameter name at registration (avoids inspect on first connection)
        try:
            from astraapi._cpp_server import precompute_ws_signature
            precompute_ws_signature(endpoint)
        except ImportError:
            pass

    def route(
        self,
        path: str,
        methods: Collection[str] | None = None,
        name: str | None = None,
        include_in_schema: bool = True,
    ) -> Callable:
        """Decorator to add an HTTP route."""
        warnings.warn(
            "The `route` decorator is deprecated, and will be removed in version 1.0.0."
            "Refer to https://starlette.dev/routing/#http-routing for the recommended approach.",
            DeprecationWarning,
            stacklevel=2,
        )

        def decorator(func: Callable) -> Callable:
            self.add_route(
                path,
                func,
                methods=methods,
                name=name,
                include_in_schema=include_in_schema,
            )
            return func

        return decorator

    def websocket_route(self, path: str, name: str | None = None) -> Callable:
        """Decorator to add a WebSocket route."""
        warnings.warn(
            "The `websocket_route` decorator is deprecated, and will be removed in version 1.0.0. Refer to "
            "https://starlette.dev/routing/#websocket-routing for the recommended approach.",
            DeprecationWarning,
            stacklevel=2,
        )

        def decorator(func: Callable) -> Callable:
            self.add_websocket_route(path, func, name=name)
            return func

        return decorator

    def add_event_handler(self, event_type: str, func: Callable[[], Any]) -> None:
        assert event_type in ("startup", "shutdown")
        if event_type == "startup":
            self.on_startup.append(func)
        else:
            self.on_shutdown.append(func)

    def on_event(self, event_type: str) -> Callable:
        warnings.warn(
            "The `on_event` decorator is deprecated, and will be removed in version 1.0.0. "
            "Refer to https://starlette.dev/lifespan/ for recommended approach.",
            DeprecationWarning,
            stacklevel=2,
        )

        def decorator(func: Callable) -> Callable:
            self.add_event_handler(event_type, func)
            return func

        return decorator

    def __eq__(self, other: Any) -> bool:
        return isinstance(other, Router) and self.routes == other.routes
