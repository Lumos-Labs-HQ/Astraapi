"""
Native Request & HTTPConnection classes — replaces starlette.requests.

Implements the full ASGI request API surface used by FastAPI routing and
dependency injection, with zero starlette imports.
"""
from __future__ import annotations

from fastapi._json_utils import json_loads as _json_loads
from http.cookies import SimpleCookie
from typing import Any, Optional
from urllib.parse import unquote, urlencode, urlparse

from fastapi._types import Receive, Scope, Send

# Optional C++ accelerated parsers
try:
    from fastapi._core_bridge import (
        parse_cookie_header as _core_parse_cookies,
        parse_query_string as _core_parse_qs,
        parse_scope_headers as _core_parse_headers,
    )

    _CORE_AVAILABLE = True
except Exception:
    _CORE_AVAILABLE = False


# ---------------------------------------------------------------------------
# Lightweight data structures
# ---------------------------------------------------------------------------


class _Address:
    """Minimal client/server address container."""

    __slots__ = ("host", "port")

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port

    def __iter__(self):
        yield self.host
        yield self.port

    def __getitem__(self, index: int):
        return (self.host, self.port)[index]

    def __repr__(self) -> str:
        return f"Address(host={self.host!r}, port={self.port})"


class URL:
    """Parsed URL with lazy component access."""

    __slots__ = ("_url", "_components")

    def __init__(self, url: str = "", *, scope: Optional[Scope] = None) -> None:
        if scope is not None:
            scheme = scope.get("scheme", "http")
            server = scope.get("server")
            path = scope.get("root_path", "") + scope.get("path", "/")
            query_string = scope.get("query_string", b"")
            if server:
                host, port = server
                default_port = {"http": 80, "https": 443, "ws": 80, "wss": 443}
                if port == default_port.get(scheme):
                    netloc = host
                else:
                    netloc = f"{host}:{port}"
                url = f"{scheme}://{netloc}{path}"
            else:
                url = path
            if query_string:
                qs = query_string if isinstance(query_string, str) else query_string.decode("latin-1")
                url = f"{url}?{qs}"
        self._url = url
        self._components = urlparse(url)

    @property
    def scheme(self) -> str:
        return self._components.scheme

    @property
    def netloc(self) -> str:
        return self._components.netloc

    @property
    def path(self) -> str:
        return self._components.path

    @property
    def query(self) -> str:
        return self._components.query

    @property
    def fragment(self) -> str:
        return self._components.fragment

    @property
    def hostname(self) -> Optional[str]:
        return self._components.hostname

    @property
    def port(self) -> Optional[int]:
        return self._components.port

    @property
    def username(self) -> Optional[str]:
        return self._components.username

    @property
    def password(self) -> Optional[str]:
        return self._components.password

    @property
    def is_secure(self) -> bool:
        return self.scheme in ("https", "wss")

    @property
    def components(self):
        return self._components

    def replace(self, **kwargs: Any) -> "URL":
        return URL(self._components._replace(**kwargs).geturl())

    def __str__(self) -> str:
        return self._url

    def __repr__(self) -> str:
        return f"URL({self._url!r})"

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, URL):
            return self._url == other._url
        return self._url == str(other)

    def __hash__(self) -> int:
        return hash(self._url)


class Headers:
    """Immutable, case-insensitive header mapping backed by ASGI raw headers."""

    __slots__ = ("_raw", "_dict", "_list")

    def __init__(
        self,
        raw: Optional[list] = None,
        scope: Optional[Scope] = None,
    ) -> None:
        if raw is not None:
            self._raw = raw
        elif scope is not None:
            self._raw = scope.get("headers", [])
        else:
            self._raw = []
        self._dict: Optional[dict[str, str]] = None
        self._list: Optional[list[tuple[str, str]]] = None

    @property
    def raw(self) -> list:
        return self._raw

    def _ensure_parsed(self) -> None:
        if self._dict is None:
            d: dict[str, str] = {}
            items: list[tuple[str, str]] = []
            for name_bytes, value_bytes in self._raw:
                name = name_bytes.decode("latin-1").lower() if isinstance(name_bytes, bytes) else name_bytes.lower()
                value = value_bytes.decode("latin-1") if isinstance(value_bytes, bytes) else value_bytes
                d[name] = value
                items.append((name, value))
            self._dict = d
            self._list = items

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        self._ensure_parsed()
        return self._dict.get(key.lower(), default)  # type: ignore[union-attr]

    def getlist(self, key: str) -> list[str]:
        self._ensure_parsed()
        key_lower = key.lower()
        return [v for k, v in self._list if k == key_lower]  # type: ignore[union-attr]

    def keys(self) -> list[str]:
        self._ensure_parsed()
        return list(self._dict.keys())  # type: ignore[union-attr]

    def values(self) -> list[str]:
        self._ensure_parsed()
        return list(self._dict.values())  # type: ignore[union-attr]

    def items(self) -> list[tuple[str, str]]:
        self._ensure_parsed()
        return list(self._dict.items())  # type: ignore[union-attr]

    def __getitem__(self, key: str) -> str:
        self._ensure_parsed()
        return self._dict[key.lower()]  # type: ignore[index]

    def __contains__(self, key: Any) -> bool:
        self._ensure_parsed()
        return key.lower() in self._dict  # type: ignore[operator]

    def __iter__(self):
        self._ensure_parsed()
        return iter(self._dict)  # type: ignore[arg-type]

    def __len__(self) -> int:
        self._ensure_parsed()
        return len(self._dict)  # type: ignore[arg-type]

    def __repr__(self) -> str:
        self._ensure_parsed()
        return f"Headers({self._dict!r})"


class QueryParams:
    """Immutable multi-value query-string mapping."""

    __slots__ = ("_raw", "_dict", "_multi")

    def __init__(self, raw: Optional[str] = None, scope: Optional[Scope] = None) -> None:
        if raw is not None:
            self._raw = raw
        elif scope is not None:
            qs = scope.get("query_string", b"")
            self._raw = qs.decode("latin-1") if isinstance(qs, bytes) else qs
        else:
            self._raw = ""
        self._dict: Optional[dict[str, str]] = None
        self._multi: Optional[dict[str, list[str]]] = None

    def _ensure_parsed(self) -> None:
        if self._dict is not None:
            return
        if _CORE_AVAILABLE and self._raw:
            multi = _core_parse_qs(self._raw)
        else:
            multi = _parse_qs_python(self._raw)
        self._multi = multi
        self._dict = {k: v[-1] for k, v in multi.items() if v}

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        self._ensure_parsed()
        return self._dict.get(key, default)  # type: ignore[union-attr]

    def getlist(self, key: str) -> list[str]:
        self._ensure_parsed()
        return self._multi.get(key, [])  # type: ignore[union-attr]

    def keys(self):
        self._ensure_parsed()
        return self._dict.keys()  # type: ignore[union-attr]

    def values(self):
        self._ensure_parsed()
        return self._dict.values()  # type: ignore[union-attr]

    def items(self):
        self._ensure_parsed()
        return self._dict.items()  # type: ignore[union-attr]

    def multi_items(self) -> list[tuple[str, str]]:
        self._ensure_parsed()
        result: list[tuple[str, str]] = []
        for k, vs in self._multi.items():  # type: ignore[union-attr]
            for v in vs:
                result.append((k, v))
        return result

    def __getitem__(self, key: str) -> str:
        self._ensure_parsed()
        return self._dict[key]  # type: ignore[index]

    def __contains__(self, key: Any) -> bool:
        self._ensure_parsed()
        return key in self._dict  # type: ignore[operator]

    def __iter__(self):
        self._ensure_parsed()
        return iter(self._dict)  # type: ignore[arg-type]

    def __len__(self) -> int:
        self._ensure_parsed()
        return len(self._dict)  # type: ignore[arg-type]

    def __repr__(self) -> str:
        self._ensure_parsed()
        return f"QueryParams({self._dict!r})"

    def __str__(self) -> str:
        return self._raw

    def __bool__(self) -> bool:
        self._ensure_parsed()
        return bool(self._dict)


class State:
    """Simple attribute-based state container."""

    __slots__ = ("_state",)

    def __init__(self, state: Optional[dict[str, Any]] = None) -> None:
        if state is None:
            state = {}
        super().__setattr__("_state", state)

    def __setattr__(self, name: str, value: Any) -> None:
        self._state[name] = value

    def __getattr__(self, name: str) -> Any:
        try:
            return self._state[name]
        except KeyError:
            message = "'{}' object has no attribute '{}'"
            raise AttributeError(message.format(self.__class__.__name__, name))

    def __delattr__(self, name: str) -> None:
        try:
            del self._state[name]
        except KeyError:
            message = "'{}' object has no attribute '{}'"
            raise AttributeError(message.format(self.__class__.__name__, name))

    def __contains__(self, key: str) -> bool:
        return key in self._state

    def __repr__(self) -> str:
        return f"State({self._state!r})"

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, State):
            return self._state == other._state
        return NotImplemented


# ---------------------------------------------------------------------------
# Pure-Python fallback query string parser
# ---------------------------------------------------------------------------

def _parse_qs_python(qs: str) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    if not qs:
        return result
    for part in qs.split("&"):
        if not part:
            continue
        if "=" in part:
            k, v = part.split("=", 1)
        else:
            k, v = part, ""
        k = unquote(k.replace("+", " "))
        v = unquote(v.replace("+", " "))
        result.setdefault(k, []).append(v)
    return result


# ---------------------------------------------------------------------------
# HTTPConnection — base class for Request and WebSocket
# ---------------------------------------------------------------------------

class HTTPConnection:
    """
    Base class providing shared properties for HTTP requests and WebSocket
    connections.  Mirrors the Starlette HTTPConnection API.
    """

    def __init__(self, scope: Scope, receive: Optional[Receive] = None, send: Optional[Send] = None) -> None:
        assert scope["type"] in ("http", "websocket")
        self._scope = scope
        self._receive = receive
        self._send = send
        # Lazy caches
        self._url: Optional[URL] = None
        self._base_url: Optional[URL] = None
        self._headers: Optional[Headers] = None
        self._query_params: Optional[QueryParams] = None
        self._cookies: Optional[dict[str, str]] = None

    @property
    def scope(self) -> Scope:
        return self._scope

    @property
    def app(self) -> Any:
        return self._scope.get("app")

    @property
    def method(self) -> str:
        return self._scope.get("method", "")

    @property
    def url(self) -> URL:
        if self._url is None:
            self._url = URL(scope=self._scope)
        return self._url

    @property
    def base_url(self) -> URL:
        if self._base_url is None:
            scheme = self._scope.get("scheme", "http")
            server = self._scope.get("server")
            root_path = self._scope.get("root_path", "")
            if server:
                host, port = server
                default_port = {"http": 80, "https": 443, "ws": 80, "wss": 443}
                if port == default_port.get(scheme):
                    netloc = host
                else:
                    netloc = f"{host}:{port}"
                self._base_url = URL(f"{scheme}://{netloc}{root_path}/")
            else:
                self._base_url = URL(f"{root_path}/")
        return self._base_url

    @property
    def headers(self) -> Headers:
        if self._headers is None:
            self._headers = Headers(scope=self._scope)
        return self._headers

    @property
    def query_params(self) -> QueryParams:
        if self._query_params is None:
            self._query_params = QueryParams(scope=self._scope)
        return self._query_params

    @property
    def path_params(self) -> dict[str, Any]:
        return self._scope.get("path_params", {})

    @path_params.setter
    def path_params(self, value: dict[str, Any]) -> None:
        self._scope["path_params"] = value

    @property
    def cookies(self) -> dict[str, str]:
        if self._cookies is None:
            cookie_header = self.headers.get("cookie", "")
            if _CORE_AVAILABLE and cookie_header:
                self._cookies = _core_parse_cookies(cookie_header)
            else:
                self._cookies = _parse_cookies_python(cookie_header)
        return self._cookies

    @property
    def client(self) -> Optional[_Address]:
        client_info = self._scope.get("client")
        if client_info is not None:
            return _Address(client_info[0], client_info[1])
        return None

    @property
    def state(self) -> State:
        if "state" not in self._scope:
            self._scope["state"] = State()
        return self._scope["state"]

    @property
    def auth(self) -> Any:
        return self._scope.get("auth")

    @property
    def user(self) -> Any:
        return self._scope.get("user")

    def url_for(self, name: str, /, **path_params: Any) -> URL:
        router = self._scope.get("router")
        if router is None:
            raise RuntimeError("Router not available in scope")
        url_path = router.url_path_for(name, **path_params)
        return URL(str(self.base_url).rstrip("/") + str(url_path))

    def __getitem__(self, key: str) -> Any:
        return self._scope[key]

    def __iter__(self):
        return iter(self._scope)

    def __len__(self) -> int:
        return len(self._scope)

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, HTTPConnection):
            return self._scope is other._scope
        return NotImplemented

    def __hash__(self) -> int:
        return id(self._scope)


def _parse_cookies_python(cookie_header: str) -> dict[str, str]:
    """Parse a Cookie header string into a dict using stdlib."""
    cookies: dict[str, str] = {}
    if not cookie_header:
        return cookies
    sc = SimpleCookie()
    try:
        sc.load(cookie_header)
    except Exception:
        return cookies
    for key, morsel in sc.items():
        cookies[key] = morsel.value
    return cookies


# ---------------------------------------------------------------------------
# Request
# ---------------------------------------------------------------------------

class Request(HTTPConnection):
    """
    Full ASGI HTTP request.  Provides body/json/form/stream async methods
    on top of the HTTPConnection base.
    """

    _disconnected: bool

    def __init__(self, scope: Scope, receive: Optional[Receive] = None, send: Optional[Send] = None) -> None:
        super().__init__(scope, receive, send)
        self._json: Any = None
        self._json_loaded: bool = False
        self._form: Any = None
        self._disconnected = False
        self._stream_consumed = False
        # Pre-populate body from scope if C++ server cached it there.
        # Use "_body" key presence (not truthiness) so empty body is also cached.
        if "_body" in scope:
            self._body = scope["_body"]

    @property
    def receive(self) -> Receive:
        assert self._receive is not None, "No receive channel available"
        return self._receive

    @property
    def send(self) -> Send:
        assert self._send is not None, "No send channel available"
        return self._send

    async def stream(self):
        """Yield body chunks from the ASGI receive channel."""
        if hasattr(self, '_body') and self._body is not None:
            yield self._body
            yield b""
            return
        if self._stream_consumed:
            raise RuntimeError("Stream already consumed")
        self._stream_consumed = True
        while True:
            message = await self.receive()
            body = message.get("body", b"")
            if body:
                yield body
            if not message.get("more_body", False):
                break

    async def body(self) -> bytes:
        """Read the entire request body."""
        if not hasattr(self, '_body') or self._body is None:
            chunks: list[bytes] = []
            async for chunk in self.stream():
                chunks.append(chunk)
            self._body = b"".join(chunks)
        return self._body

    async def json(self) -> Any:
        """Parse request body as JSON."""
        if not self._json_loaded:
            body = await self.body()
            self._json = _json_loads(body)
            self._json_loaded = True
        return self._json

    async def form(self, *, max_files: int = 1000, max_fields: int = 1000) -> Any:
        """Parse request body as form data.

        Returns a dict-like object.  For full multipart support the
        ``python-multipart`` package must be installed.
        """
        if self._form is None:
            content_type = self.headers.get("content-type", "")
            body = await self.body()
            if "multipart/form-data" in content_type:
                try:
                    from fastapi._core_bridge import parse_multipart_body
                    self._form = parse_multipart_body(body, content_type)
                except Exception:
                    # Fallback: attempt python-multipart
                    self._form = await _parse_multipart_fallback(
                        body, content_type, max_files=max_files, max_fields=max_fields
                    )
            elif "application/x-www-form-urlencoded" in content_type:
                try:
                    from fastapi._core_bridge import parse_urlencoded_body
                    self._form = parse_urlencoded_body(body)
                except Exception:
                    self._form = dict(_parse_qs_python(body.decode("latin-1")))
            else:
                self._form = {}
        return self._form

    async def close(self) -> None:
        """Clean up form data (upload files etc.)."""
        if self._form is not None and hasattr(self._form, "close"):
            await self._form.close()

    async def is_disconnected(self) -> bool:
        """Check if the client has disconnected."""
        if self._disconnected:
            return True
        # Non-blocking check
        try:
            message = await self.receive()
            if message.get("type") == "http.disconnect":
                self._disconnected = True
        except Exception:
            self._disconnected = True
        return self._disconnected


async def _parse_multipart_fallback(
    body: bytes,
    content_type: str,
    *,
    max_files: int = 1000,
    max_fields: int = 1000,
) -> dict[str, Any]:
    """Fallback multipart parser using python-multipart."""
    try:
        from python_multipart.multipart import parse_options_header
        import python_multipart.multipart as multipart
    except ImportError:
        # Last resort: return empty
        return {}

    content_type_bytes, options = parse_options_header(content_type)
    boundary = options.get(b"boundary", b"")

    if not boundary:
        return {}

    fields: dict[str, Any] = {}

    class _Parser:
        def __init__(self) -> None:
            self.current_name: Optional[str] = None
            self.current_data = bytearray()

        def on_part_begin(self) -> None:
            self.current_data = bytearray()

        def on_part_data(self, data: bytes, start: int, end: int) -> None:
            self.current_data.extend(data[start:end])

        def on_part_end(self) -> None:
            if self.current_name is not None:
                fields[self.current_name] = bytes(self.current_data)
            self.current_name = None

        def on_header_value(self, data: bytes, start: int, end: int) -> None:
            header_value = data[start:end].decode("latin-1")
            if 'name="' in header_value:
                name_start = header_value.index('name="') + 6
                name_end = header_value.index('"', name_start)
                self.current_name = header_value[name_start:name_end]

    # Simple fallback: just parse as URL-encoded if multipart fails
    try:
        parser_instance = _Parser()
        mp_parser = multipart.MultipartParser(
            boundary,
            {
                "on_part_begin": parser_instance.on_part_begin,
                "on_part_data": parser_instance.on_part_data,
                "on_part_end": parser_instance.on_part_end,
                "on_header_value": parser_instance.on_header_value,
            },
        )
        mp_parser.write(body)
        mp_parser.finalize()
    except Exception:
        pass

    return fields
