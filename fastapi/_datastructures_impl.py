"""
Data structure implementations -- replaces starlette.datastructures.

Provides State, URL, Address, Headers, QueryParams, FormData, and
UploadFile with zero starlette imports.
"""
from __future__ import annotations

import io
import tempfile
from typing import Any, Iterator, Optional, Sequence, Tuple, Union
from urllib.parse import unquote, urlparse

from fastapi._concurrency import run_in_threadpool


# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------


class State:
    """Simple attribute-based state container.

    Stores arbitrary attributes in an internal ``_state`` dict, providing
    attribute-style access (``state.foo = 1``) and dict-like containment
    checks (``"foo" in state``).
    """

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

    def __copy__(self) -> "State":
        return State(dict(self._state))


# ---------------------------------------------------------------------------
# URL
# ---------------------------------------------------------------------------


class URL:
    """Parsed URL wrapper using ``urllib.parse.urlparse``.

    Parameters
    ----------
    url : str
        The URL string to parse.
    """

    __slots__ = ("_url", "_components")

    def __init__(self, url: str = "") -> None:
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
        """Return a new URL with the specified components replaced."""
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


# ---------------------------------------------------------------------------
# Address
# ---------------------------------------------------------------------------


class Address:
    """Network address container with ``host`` and ``port``.

    Supports tuple-style unpacking and indexing for backwards
    compatibility with code expecting ``(host, port)`` tuples.
    """

    __slots__ = ("host", "port")

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port

    def __iter__(self) -> Iterator:
        yield self.host
        yield self.port

    def __getitem__(self, index: int) -> Any:
        return (self.host, self.port)[index]

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, Address):
            return self.host == other.host and self.port == other.port
        if isinstance(other, tuple) and len(other) == 2:
            return self.host == other[0] and self.port == other[1]
        return NotImplemented

    def __repr__(self) -> str:
        return f"Address(host={self.host!r}, port={self.port})"

    def __hash__(self) -> int:
        return hash((self.host, self.port))


# ---------------------------------------------------------------------------
# ImmutableMultiDict — base for Headers and QueryParams
# ---------------------------------------------------------------------------


class ImmutableMultiDict:
    """Abstract base providing getlist() semantics for isinstance checks."""

    def getlist(self, key: str) -> list:  # pragma: no cover
        raise NotImplementedError


# ---------------------------------------------------------------------------
# Headers
# ---------------------------------------------------------------------------


class Headers(ImmutableMultiDict):
    """Immutable, case-insensitive header mapping backed by ASGI raw headers.

    Parameters
    ----------
    raw : list of (bytes, bytes) tuples, optional
        Raw ASGI header pairs.
    scope : dict, optional
        ASGI scope from which to read ``headers``.
    headers : dict of str to str, optional
        Convenience initializer from a plain dict.
    """

    __slots__ = ("_raw", "_dict", "_list")

    def __init__(
        self,
        raw: Optional[list] = None,
        scope: Optional[dict] = None,
        headers: Optional[dict[str, str]] = None,
    ) -> None:
        if raw is not None:
            self._raw = raw
        elif scope is not None:
            self._raw = scope.get("headers", [])
        elif headers is not None:
            self._raw = [
                (k.lower().encode("latin-1"), v.encode("latin-1"))
                for k, v in headers.items()
            ]
        else:
            self._raw = []
        self._dict: Optional[dict[str, str]] = None
        self._list: Optional[list[tuple[str, str]]] = None

    @property
    def raw(self) -> list:
        """Return the underlying raw header list."""
        return self._raw

    def _ensure_parsed(self) -> None:
        if self._dict is not None:
            return
        d: dict[str, str] = {}
        items: list[tuple[str, str]] = []
        for name_bytes, value_bytes in self._raw:
            name = (
                name_bytes.decode("latin-1").lower()
                if isinstance(name_bytes, bytes)
                else name_bytes.lower()
            )
            value = (
                value_bytes.decode("latin-1")
                if isinstance(value_bytes, bytes)
                else value_bytes
            )
            d.setdefault(name, value)
            items.append((name, value))
        self._dict = d
        self._list = items

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        self._ensure_parsed()
        return self._dict.get(key.lower(), default)  # type: ignore[union-attr]

    def getlist(self, key: str) -> list[str]:
        """Return all values for the given header name."""
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

    def multi_items(self) -> list[tuple[str, str]]:
        """Return all header name-value pairs including duplicates."""
        self._ensure_parsed()
        return list(self._list)  # type: ignore[arg-type]

    def __getitem__(self, key: str) -> str:
        self._ensure_parsed()
        try:
            return self._dict[key.lower()]  # type: ignore[index]
        except KeyError:
            raise KeyError(key)

    def __contains__(self, key: Any) -> bool:
        self._ensure_parsed()
        return key.lower() in self._dict  # type: ignore[operator]

    def __iter__(self) -> Iterator[str]:
        self._ensure_parsed()
        return iter(self._dict)  # type: ignore[arg-type]

    def __len__(self) -> int:
        self._ensure_parsed()
        return len(self._dict)  # type: ignore[arg-type]

    def __repr__(self) -> str:
        self._ensure_parsed()
        return f"Headers({self._dict!r})"

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, Headers):
            self._ensure_parsed()
            other._ensure_parsed()
            return self._dict == other._dict
        return NotImplemented


# ---------------------------------------------------------------------------
# QueryParams
# ---------------------------------------------------------------------------


class QueryParams(ImmutableMultiDict):
    """Immutable multi-value query-string mapping.

    Parameters
    ----------
    query_string : str, optional
        Raw query string (without leading ``?``).
    """

    __slots__ = ("_raw", "_dict", "_multi")

    def __init__(self, query_string: str = "") -> None:
        if isinstance(query_string, bytes):
            query_string = query_string.decode("latin-1")
        self._raw = query_string
        self._dict: Optional[dict[str, str]] = None
        self._multi: Optional[dict[str, list[str]]] = None

    def _ensure_parsed(self) -> None:
        if self._dict is not None:
            return
        multi: dict[str, list[str]] = {}
        if self._raw:
            for part in self._raw.split("&"):
                if not part:
                    continue
                if "=" in part:
                    k, v = part.split("=", 1)
                else:
                    k, v = part, ""
                k = unquote(k.replace("+", " "))
                v = unquote(v.replace("+", " "))
                multi.setdefault(k, []).append(v)
        self._multi = multi
        self._dict = {k: v[-1] for k, v in multi.items() if v}

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        self._ensure_parsed()
        return self._dict.get(key, default)  # type: ignore[union-attr]

    def getlist(self, key: str) -> list[str]:
        """Return all values for the given query parameter name."""
        self._ensure_parsed()
        return self._multi.get(key, [])  # type: ignore[union-attr]

    def multi_items(self) -> list[tuple[str, str]]:
        """Return all key-value pairs including duplicates."""
        self._ensure_parsed()
        result: list[tuple[str, str]] = []
        for k, vs in self._multi.items():  # type: ignore[union-attr]
            for v in vs:
                result.append((k, v))
        return result

    def keys(self):
        self._ensure_parsed()
        return self._dict.keys()  # type: ignore[union-attr]

    def values(self):
        self._ensure_parsed()
        return self._dict.values()  # type: ignore[union-attr]

    def items(self):
        self._ensure_parsed()
        return self._dict.items()  # type: ignore[union-attr]

    def __getitem__(self, key: str) -> str:
        self._ensure_parsed()
        return self._dict[key]  # type: ignore[index]

    def __contains__(self, key: Any) -> bool:
        self._ensure_parsed()
        return key in self._dict  # type: ignore[operator]

    def __iter__(self) -> Iterator[str]:
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

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, QueryParams):
            self._ensure_parsed()
            other._ensure_parsed()
            return self._multi == other._multi
        return NotImplemented


# ---------------------------------------------------------------------------
# UploadFile
# ---------------------------------------------------------------------------


class UploadFile:
    """An uploaded file backed by a ``SpooledTemporaryFile``.

    Parameters
    ----------
    filename : str or None
        The original filename from the upload.
    file : file-like or None
        An existing file object.  If ``None``, a new
        ``SpooledTemporaryFile`` is created.
    content_type : str
        MIME type (default ``application/octet-stream``).
    headers : Headers or None
        Headers associated with the file part.
    size : int or None
        File size in bytes if known.
    """

    spool_max_size: int = 1024 * 1024  # 1 MiB before rolling to disk

    def __init__(
        self,
        filename: Optional[str] = None,
        file: Optional[Any] = None,
        content_type: str = "application/octet-stream",
        headers: Optional[Headers] = None,
        size: Optional[int] = None,
    ) -> None:
        self.filename = filename
        self.content_type = content_type
        self.headers = headers or Headers()
        self.size = size
        if file is None:
            self.file = tempfile.SpooledTemporaryFile(
                max_size=self.spool_max_size, mode="w+b"
            )
        else:
            self.file = file

    async def read(self, size: int = -1) -> bytes:
        """Read up to *size* bytes from the file."""
        return await run_in_threadpool(self.file.read, size)

    async def write(self, data: bytes) -> None:
        """Write *data* to the file."""
        await run_in_threadpool(self.file.write, data)

    async def seek(self, offset: int) -> None:
        """Seek to *offset* in the file."""
        await run_in_threadpool(self.file.seek, offset)

    async def close(self) -> None:
        """Close the underlying file object."""
        await run_in_threadpool(self.file.close)

    def __repr__(self) -> str:
        return (
            f"UploadFile(filename={self.filename!r}, "
            f"content_type={self.content_type!r}, "
            f"size={self.size})"
        )

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, UploadFile):
            return (
                self.filename == other.filename
                and self.content_type == other.content_type
                and self.size == other.size
            )
        return NotImplemented


# ---------------------------------------------------------------------------
# FormData
# ---------------------------------------------------------------------------


class FormData:
    """Multi-value mapping for form data.

    Supports both plain string values and ``UploadFile`` instances.
    Provides an ``async close()`` method for cleaning up uploaded files.

    Parameters
    ----------
    *args, **kwargs :
        Accepts a dict, a sequence of (key, value) pairs, or keyword
        arguments for initialisation.
    """

    __slots__ = ("_dict", "_list")

    def __init__(
        self,
        *args: Any,
        **kwargs: Any,
    ) -> None:
        items: list[tuple[str, Any]] = []
        if args:
            data = args[0]
            if isinstance(data, dict):
                items = list(data.items())
            elif isinstance(data, (list, tuple)):
                items = list(data)
            elif isinstance(data, FormData):
                items = list(data.multi_items())
            else:
                items = list(data)
        if kwargs:
            items.extend(kwargs.items())
        self._list: list[tuple[str, Any]] = items
        self._dict: dict[str, Any] = {}
        for k, v in items:
            self._dict.setdefault(k, v)

    def get(self, key: str, default: Any = None) -> Any:
        return self._dict.get(key, default)

    def getlist(self, key: str) -> list[Any]:
        """Return all values for the given field name."""
        return [v for k, v in self._list if k == key]

    def multi_items(self) -> list[tuple[str, Any]]:
        """Return all key-value pairs including duplicates."""
        return list(self._list)

    def keys(self):
        return self._dict.keys()

    def values(self):
        return self._dict.values()

    def items(self):
        return self._dict.items()

    def __getitem__(self, key: str) -> Any:
        return self._dict[key]

    def __contains__(self, key: Any) -> bool:
        return key in self._dict

    def __iter__(self) -> Iterator[str]:
        return iter(self._dict)

    def __len__(self) -> int:
        return len(self._dict)

    def __repr__(self) -> str:
        return f"FormData(items={self._list!r})"

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, FormData):
            return self._list == other._list
        return NotImplemented

    def __bool__(self) -> bool:
        return bool(self._list)

    async def close(self) -> None:
        """Close all ``UploadFile`` values."""
        for _, value in self._list:
            if isinstance(value, UploadFile):
                await value.close()
