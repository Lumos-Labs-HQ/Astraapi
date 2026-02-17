"""
Application base class -- replaces starlette.applications.Starlette.

Provides the ``AppBase`` class, which wires up routing, middleware,
exception handling, and ASGI lifespan with zero starlette imports.
"""
from __future__ import annotations

from typing import Any, Callable, Optional, Sequence, Type, Union

from fastapi._types import ASGIApp, Receive, Scope, Send
from fastapi._middleware_impl import (
    Middleware,
    ServerErrorMiddleware,
    ExceptionMiddleware,
)
from fastapi._routing_base import Router
from fastapi._datastructures_impl import State


class AppBase:
    """Minimal ASGI application base that mirrors ``starlette.applications.Starlette``.

    Composes a ``Router`` with a middleware stack (ServerErrorMiddleware at the
    outermost layer, user middleware in between, and ExceptionMiddleware closest
    to the router) and provides convenience helpers for route and middleware
    registration.

    Subclasses (e.g. ``FastAPI``) are expected to set the following instance
    attributes in their own ``__init__``:

    - ``debug``: bool
    - ``state``: State
    - ``router``: Router
    - ``routes``: list
    - ``exception_handlers``: dict
    - ``middleware_stack``: ASGIApp | None
    """

    def __init__(
        self,
        debug: bool = False,
        routes: Optional[Sequence[Any]] = None,
        middleware: Optional[Sequence[Middleware]] = None,
        exception_handlers: Optional[dict[Any, Callable[..., Any]]] = None,
        on_startup: Optional[Sequence[Callable[[], Any]]] = None,
        on_shutdown: Optional[Sequence[Callable[[], Any]]] = None,
        lifespan: Optional[Callable[..., Any]] = None,
    ) -> None:
        self.debug = debug
        self.state = State()
        self.exception_handlers: dict[Any, Callable[..., Any]] = (
            dict(exception_handlers) if exception_handlers else {}
        )

        # Create the router
        self.router = Router(
            routes=routes,
            on_startup=on_startup,
            on_shutdown=on_shutdown,
            lifespan=lifespan,
        )

        self.user_middleware: list[Middleware] = list(middleware) if middleware else []
        self.middleware_stack: Union[ASGIApp, None] = None

    @property
    def routes(self) -> list:
        return self.router.routes

    @routes.setter
    def routes(self, value: list) -> None:
        self.router.routes = value

    # -- Route registration --------------------------------------------------

    def add_route(
        self,
        path: str,
        route: Callable[..., Any],
        methods: Optional[Sequence[str]] = None,
        name: Optional[str] = None,
        include_in_schema: bool = True,
    ) -> None:
        """Add an HTTP route."""
        self.router.add_route(
            path, route, methods=methods, name=name,
            include_in_schema=include_in_schema,
        )
        self.middleware_stack = None

    def add_websocket_route(
        self,
        path: str,
        route: Callable[..., Any],
        name: Optional[str] = None,
    ) -> None:
        """Add a WebSocket route."""
        self.router.add_websocket_route(path, route, name=name)
        self.middleware_stack = None

    def mount(
        self,
        path: str,
        app: ASGIApp,
        name: Optional[str] = None,
    ) -> None:
        """Mount a sub-application at the given path."""
        self.router.mount(path, app=app, name=name)
        self.middleware_stack = None

    # -- Middleware registration ----------------------------------------------

    def add_middleware(
        self,
        middleware_class: type,
        *args: Any,
        **kwargs: Any,
    ) -> None:
        """Register a middleware class with optional arguments."""
        if args:
            self.user_middleware.insert(0, Middleware(middleware_class, *args, **kwargs))
        else:
            self.user_middleware.insert(0, Middleware(middleware_class, **kwargs))
        self.middleware_stack = None

    # -- Exception handler registration --------------------------------------

    def add_exception_handler(
        self,
        exc_class_or_status: Union[int, Type[Exception]],
        handler: Callable[..., Any],
    ) -> None:
        """Register an exception handler for the given exception class or
        HTTP status code."""
        self.exception_handlers[exc_class_or_status] = handler
        self.middleware_stack = None

    # -- Middleware stack construction ----------------------------------------

    def build_middleware_stack(self) -> ASGIApp:
        """Build the ASGI middleware chain."""
        debug = self.debug
        error_handler = None
        exception_handlers: dict[Any, Callable[..., Any]] = {}

        for key, value in self.exception_handlers.items():
            if key in (500, Exception):
                error_handler = value
            else:
                exception_handlers[key] = value

        app: ASGIApp = self.router

        app = ExceptionMiddleware(app, handlers=exception_handlers, debug=debug)

        for middleware in reversed(self.user_middleware):
            cls, args, kwargs = middleware
            app = cls(app, *args, **kwargs)

        app = ServerErrorMiddleware(app, handler=error_handler, debug=debug)

        return app

    # -- ASGI interface ------------------------------------------------------

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        """ASGI entry point -- delegates to the middleware stack."""
        scope["app"] = self
        if self.middleware_stack is None:
            self.middleware_stack = self.build_middleware_stack()
        await self.middleware_stack(scope, receive, send)

    # -- Event handler convenience (legacy) ----------------------------------

    def on_event(self, event_type: str) -> Callable:
        """Decorator to register startup/shutdown event handlers."""
        return self.router.on_event(event_type)

    def add_event_handler(self, event_type: str, func: Callable[[], Any]) -> None:
        """Register a startup/shutdown event handler."""
        self.router.add_event_handler(event_type, func)

    # -- URL generation ------------------------------------------------------

    def url_path_for(self, name: str, /, **path_params: Any) -> Any:
        """Generate a URL path for the named route."""
        return self.router.url_path_for(name, **path_params)

    # -- Route / WebSocket decorators ----------------------------------------

    def route(
        self,
        path: str,
        methods: Optional[Sequence[str]] = None,
        name: Optional[str] = None,
        include_in_schema: bool = True,
    ) -> Callable:
        """Decorator to add an HTTP route."""

        def decorator(func: Callable) -> Callable:
            self.add_route(
                path, func, methods=methods, name=name,
                include_in_schema=include_in_schema,
            )
            return func

        return decorator

    def websocket_route(
        self,
        path: str,
        name: Optional[str] = None,
    ) -> Callable:
        """Decorator to add a WebSocket route."""

        def decorator(func: Callable) -> Callable:
            self.add_websocket_route(path, func, name=name)
            return func

        return decorator
