"""ASGI type definitions — replaces starlette.types."""
from typing import Any, Callable, Coroutine, MutableMapping

Scope = MutableMapping[str, Any]
Receive = Callable[[], Coroutine[Any, Any, Any]]
Send = Callable[[Any], Coroutine[Any, Any, Any]]
ASGIApp = Callable[[Scope, Receive, Send], Coroutine[Any, Any, None]]
AppType = ASGIApp
Lifespan = Callable[..., Any]
ExceptionHandler = Callable[..., Any]
