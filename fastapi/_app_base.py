"""
Application base class -- replaces starlette.applications.Starlette.

Provides the ``AppBase`` class, which wires up routing, middleware,
exception handling, and lifespan with zero starlette imports.
"""
from __future__ import annotations

from typing import Any, Callable, Optional, Sequence, Type, Union

from fastapi._middleware_impl import Middleware
from fastapi._routing_base import Router
from fastapi._datastructures_impl import State


class AppBase:
    """Application base that mirrors ``starlette.applications.Starlette``.

    Composes a ``Router`` with middleware config and exception handlers,
    and provides convenience helpers for route and middleware registration.
    The C++ server reads middleware config directly from ``user_middleware``.
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

    def add_websocket_route(
        self,
        path: str,
        route: Callable[..., Any],
        name: Optional[str] = None,
    ) -> None:
        """Add a WebSocket route."""
        self.router.add_websocket_route(path, route, name=name)

    def mount(
        self,
        path: str,
        app: Any,
        name: Optional[str] = None,
    ) -> None:
        """Mount a sub-application at the given path."""
        self.router.mount(path, app=app, name=name)

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

    # -- Exception handler registration --------------------------------------

    def add_exception_handler(
        self,
        exc_class_or_status: Union[int, Type[Exception]],
        handler: Callable[..., Any],
    ) -> None:
        """Register an exception handler for the given exception class or
        HTTP status code."""
        self.exception_handlers[exc_class_or_status] = handler

    # -- Event handler convenience (legacy) ----------------------------------

    def on_event(self, event_type: str) -> Callable:
        """Decorator to register startup/shutdown event handlers."""
        return self.router.on_event(event_type)

    def add_event_handler(self, event_type: str, func: Callable[[], Any]) -> None:
        """Register a startup/shutdown event handler."""
        self.router.add_event_handler(event_type, func)

    # -- ASGI interface -------------------------------------------------------

    async def __call__(self, scope: Any, receive: Any, send: Any) -> None:
        """ASGI interface — delegates to the router for Starlette TestClient compat."""
        from contextlib import AsyncExitStack
        if scope.get("type") == "http" and "fastapi_middleware_astack" not in scope:
            scope["fastapi_middleware_astack"] = AsyncExitStack()
        await self.router(scope, receive, send)

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
