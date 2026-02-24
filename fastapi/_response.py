"""
Native Response classes — replaces starlette.responses.

Provides full ASGI-compatible response types with zero starlette imports.
Uses C++ ``encode_to_json_bytes`` when available for JSON serialization.
"""
from __future__ import annotations

import asyncio
from fastapi._json_utils import json_dumps as _json_dumps
import mimetypes
import os
import stat
from email.utils import formatdate
from typing import Any, AsyncIterator, Iterator, Mapping, Optional, Sequence, Union
from urllib.parse import quote

from fastapi._types import Scope

# Optional C++ accelerated JSON serialization
try:
    from fastapi._core_bridge import encode_to_json_bytes as _core_json_bytes

    _CORE_JSON = True
except Exception:
    _CORE_JSON = False


# ---------------------------------------------------------------------------
# MutableHeaders — thin wrapper over ASGI raw header list
# ---------------------------------------------------------------------------


class MutableHeaders:
    """Mutable header mapping backed by an ASGI raw header list.

    This can wrap either a ``scope`` dict (reading/writing its ``headers`` key)
    or an existing raw header list directly.

    Uses an O(1) index dict for fast lookup/set instead of O(n) linear scan.
    """

    def __init__(
        self,
        raw: Optional[list] = None,
        scope: Optional[Scope] = None,
    ) -> None:
        if raw is not None:
            self._raw = raw
        elif scope is not None:
            self._raw = scope.setdefault("headers", [])
        else:
            self._raw = []
        # Build O(1) index: lowercase header name bytes -> position in _raw
        self._index: dict[bytes, int] = {}
        for i, (name, _) in enumerate(self._raw):
            key = (name if isinstance(name, bytes) else name.encode("latin-1")).lower()
            self._index[key] = i

    @property
    def raw(self) -> list:
        return self._raw

    # -- read helpers --

    def _find(self, key: str) -> int:
        key_lower = key.lower().encode("latin-1") if isinstance(key, str) else key.lower()
        return self._index.get(key_lower, -1)

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        idx = self._find(key)
        if idx == -1:
            return default
        value = self._raw[idx][1]
        return value.decode("latin-1") if isinstance(value, bytes) else value

    def getlist(self, key: str) -> list[str]:
        key_lower = key.lower().encode("latin-1")
        result = []
        for name, value in self._raw:
            raw_name = name if isinstance(name, bytes) else name.encode("latin-1")
            if raw_name.lower() == key_lower:
                result.append(value.decode("latin-1") if isinstance(value, bytes) else value)
        return result

    def keys(self) -> list[str]:
        seen: set[str] = set()
        result: list[str] = []
        for name, _ in self._raw:
            n = name.decode("latin-1") if isinstance(name, bytes) else name
            n_lower = n.lower()
            if n_lower not in seen:
                seen.add(n_lower)
                result.append(n_lower)
        return result

    def values(self) -> list[str]:
        return [v.decode("latin-1") if isinstance(v, bytes) else v for _, v in self._raw]

    def items(self) -> list[tuple[str, str]]:
        return [
            (
                n.decode("latin-1") if isinstance(n, bytes) else n,
                v.decode("latin-1") if isinstance(v, bytes) else v,
            )
            for n, v in self._raw
        ]

    # -- write helpers --

    def __setitem__(self, key: str, value: str) -> None:
        key_bytes = key.lower().encode("latin-1")
        value_bytes = value.encode("latin-1")
        idx = self._index.get(key_bytes, -1)
        if idx >= 0:
            # O(1) in-place replace
            self._raw[idx] = (key_bytes, value_bytes)
        else:
            # Append and index
            self._index[key_bytes] = len(self._raw)
            self._raw.append((key_bytes, value_bytes))

    def __getitem__(self, key: str) -> str:
        idx = self._find(key)
        if idx == -1:
            raise KeyError(key)
        value = self._raw[idx][1]
        return value.decode("latin-1") if isinstance(value, bytes) else value

    def __delitem__(self, key: str) -> None:
        key_bytes = key.lower().encode("latin-1")
        idx = self._index.pop(key_bytes, -1)
        if idx == -1:
            raise KeyError(key)
        del self._raw[idx]
        # Rebuild index for entries after the deleted one
        for k, v in self._index.items():
            if v > idx:
                self._index[k] = v - 1

    def __contains__(self, key: Any) -> bool:
        return self._find(key) != -1

    def __iter__(self):
        return iter(self.keys())

    def __len__(self) -> int:
        return len(self._raw)

    def __repr__(self) -> str:
        return f"MutableHeaders({self.items()!r})"

    def append(self, key: str, value: str) -> None:
        """Append a header value without removing existing entries."""
        key_bytes = key.lower().encode("latin-1")
        self._index[key_bytes] = len(self._raw)
        self._raw.append((key_bytes, value.encode("latin-1")))

    def update(self, other: Union[Mapping[str, str], Sequence[tuple[str, str]]]) -> None:
        if isinstance(other, Mapping):
            pairs = other.items()
        else:
            pairs = other
        for key, value in pairs:
            self[key] = value


# ---------------------------------------------------------------------------
# Response — base ASGI response
# ---------------------------------------------------------------------------


class Response:
    """Base ASGI response class.

    Parameters
    ----------
    content : bytes | str | None
        Response body.
    status_code : int
        HTTP status code (default 200).
    headers : dict | None
        Additional response headers.
    media_type : str | None
        Content-Type media type.
    background : Any
        Background task to run after the response is sent.
    """

    media_type: Optional[str] = None
    charset: str = "utf-8"

    def __init__(
        self,
        content: Any = None,
        status_code: int = 200,
        headers: Optional[dict[str, str]] = None,
        media_type: Optional[str] = None,
        background: Any = None,
    ) -> None:
        self.status_code = status_code
        if media_type is not None:
            self.media_type = media_type
        self.background = background
        self.body = self.render(content)
        # Build raw header list
        raw_headers: list[tuple[bytes, bytes]] = []
        if headers:
            for k, v in headers.items():
                raw_headers.append((k.lower().encode("latin-1"), v.encode("latin-1")))
        self._raw_headers = raw_headers
        # Populate content-type and content-length
        self._populate_content_headers()

    def _populate_content_headers(self) -> None:
        """Ensure content-type and content-length headers are set."""
        body = self.body
        # Build content-type
        if self.media_type is not None:
            content_type = self.media_type
            if content_type.startswith("text/") and "charset=" not in content_type:
                content_type += f"; charset={self.charset}"
            self._set_raw_header(b"content-type", content_type.encode("latin-1"))
        # Content-length
        if body is not None:
            self._set_raw_header(b"content-length", str(len(body)).encode("latin-1"))

    def _set_raw_header(self, name: bytes, value: bytes) -> None:
        """Set a raw header, replacing any existing entry with the same name."""
        name_lower = name.lower()
        for i, (n, _) in enumerate(self._raw_headers):
            if n.lower() == name_lower:
                self._raw_headers[i] = (name_lower, value)
                return
        self._raw_headers.append((name_lower, value))

    def render(self, content: Any) -> bytes:
        """Render content to bytes."""
        if content is None:
            return b""
        if isinstance(content, bytes):
            return content
        return content.encode(self.charset)

    @property
    def headers(self) -> MutableHeaders:
        return MutableHeaders(raw=self._raw_headers)

    def set_cookie(
        self,
        key: str,
        value: str = "",
        max_age: Optional[int] = None,
        expires: Optional[int] = None,
        path: str = "/",
        domain: Optional[str] = None,
        secure: bool = False,
        httponly: bool = False,
        samesite: Optional[str] = "lax",
    ) -> None:
        """Append a Set-Cookie header."""
        cookie_val = f"{quote(key)}={quote(value)}"
        if max_age is not None:
            cookie_val += f"; Max-Age={max_age}"
        if expires is not None:
            cookie_val += f"; Expires={expires}"
        if path:
            cookie_val += f"; Path={path}"
        if domain:
            cookie_val += f"; Domain={domain}"
        if secure:
            cookie_val += "; Secure"
        if httponly:
            cookie_val += "; HttpOnly"
        if samesite:
            cookie_val += f"; SameSite={samesite}"
        self._raw_headers.append(
            (b"set-cookie", cookie_val.encode("latin-1"))
        )

    def delete_cookie(
        self,
        key: str,
        path: str = "/",
        domain: Optional[str] = None,
        secure: bool = False,
        httponly: bool = False,
        samesite: Optional[str] = "lax",
    ) -> None:
        """Delete a cookie by setting its expiry in the past."""
        self.set_cookie(
            key,
            value="",
            max_age=0,
            expires=0,
            path=path,
            domain=domain,
            secure=secure,
            httponly=httponly,
            samesite=samesite,
        )


# ---------------------------------------------------------------------------
# JSONResponse
# ---------------------------------------------------------------------------


class JSONResponse(Response):
    """JSON response with optional C++ fast-path serialization."""

    media_type = "application/json"

    def __init__(
        self,
        content: Any = None,
        status_code: int = 200,
        headers: Optional[dict[str, str]] = None,
        media_type: Optional[str] = None,
        background: Any = None,
    ) -> None:
        super().__init__(content, status_code, headers, media_type, background)

    def render(self, content: Any) -> bytes:
        if content is None:
            return b"null"
        # Try C++ fast-path
        if _CORE_JSON:
            try:
                return _core_json_bytes(content)
            except (ValueError, TypeError):
                pass
        return _json_dumps(content)


# ---------------------------------------------------------------------------
# HTMLResponse
# ---------------------------------------------------------------------------


class HTMLResponse(Response):
    """HTML response — sets content-type to text/html."""

    media_type = "text/html"


# ---------------------------------------------------------------------------
# PlainTextResponse
# ---------------------------------------------------------------------------


class PlainTextResponse(Response):
    """Plain text response — sets content-type to text/plain."""

    media_type = "text/plain"


# ---------------------------------------------------------------------------
# RedirectResponse
# ---------------------------------------------------------------------------


class RedirectResponse(Response):
    """HTTP redirect response."""

    def __init__(
        self,
        url: str,
        status_code: int = 307,
        headers: Optional[dict[str, str]] = None,
        background: Any = None,
    ) -> None:
        merged_headers = dict(headers) if headers else {}
        merged_headers["location"] = quote(str(url), safe=":/%#?=@[]!$&'()*+,;")
        super().__init__(
            content=b"",
            status_code=status_code,
            headers=merged_headers,
            background=background,
        )


# ---------------------------------------------------------------------------
# StreamingResponse
# ---------------------------------------------------------------------------


class StreamingResponse(Response):
    """ASGI streaming response backed by an async or sync iterator."""

    def __init__(
        self,
        content: Any,
        status_code: int = 200,
        headers: Optional[dict[str, str]] = None,
        media_type: Optional[str] = None,
        background: Any = None,
    ) -> None:
        self.status_code = status_code
        if media_type is not None:
            self.media_type = media_type
        self.background = background
        self.body_iterator = content

        # Build raw header list (no body rendering — we stream instead)
        raw_headers: list[tuple[bytes, bytes]] = []
        if headers:
            for k, v in headers.items():
                raw_headers.append((k.lower().encode("latin-1"), v.encode("latin-1")))
        self._raw_headers = raw_headers
        self.body = b""

        # Set content-type if specified
        if self.media_type is not None:
            content_type = self.media_type
            if content_type.startswith("text/") and "charset=" not in content_type:
                content_type += f"; charset={self.charset}"
            self._set_raw_header(b"content-type", content_type.encode("latin-1"))


# ---------------------------------------------------------------------------
# FileResponse
# ---------------------------------------------------------------------------


class FileResponse(Response):
    """Serve a file with proper content headers.

    Sends the file using streaming to avoid loading the entire file into
    memory at once.
    """

    chunk_size = 64 * 1024  # 64 KiB

    def __init__(
        self,
        path: str,
        status_code: int = 200,
        headers: Optional[dict[str, str]] = None,
        media_type: Optional[str] = None,
        background: Any = None,
        filename: Optional[str] = None,
        stat_result: Optional[os.stat_result] = None,
        method: Optional[str] = None,
        content_disposition_type: str = "attachment",
    ) -> None:
        self.path = path
        self.filename = filename
        self.stat_result = stat_result
        self.send_header_only = method is not None and method.upper() == "HEAD"
        self.content_disposition_type = content_disposition_type

        self.status_code = status_code
        self.background = background
        self.body = b""

        if media_type is None:
            media_type = _guess_media_type(filename or path)
        self.media_type = media_type

        # Build raw headers
        raw_headers: list[tuple[bytes, bytes]] = []
        if headers:
            for k, v in headers.items():
                raw_headers.append((k.lower().encode("latin-1"), v.encode("latin-1")))
        self._raw_headers = raw_headers

    def _populate_file_headers(self) -> None:
        """Set content-type, content-length, content-disposition, last-modified, etag."""
        # content-type
        if self.media_type is not None:
            content_type = self.media_type
            if content_type.startswith("text/") and "charset=" not in content_type:
                content_type += f"; charset={self.charset}"
            self._set_raw_header(b"content-type", content_type.encode("latin-1"))

        # stat the file
        if self.stat_result is None:
            self.stat_result = os.stat(self.path)
        st = self.stat_result

        # content-length
        self._set_raw_header(b"content-length", str(st.st_size).encode("latin-1"))

        # last-modified
        last_modified = formatdate(st.st_mtime, usegmt=True)
        self._set_raw_header(b"last-modified", last_modified.encode("latin-1"))

        # etag
        etag = f'"{int(st.st_mtime):x}-{st.st_size:x}"'
        self._set_raw_header(b"etag", etag.encode("latin-1"))

        # content-disposition
        if self.filename is not None:
            content_disp = '{}; filename="{}"'.format(
                self.content_disposition_type, self.filename
            )
            self._set_raw_header(
                b"content-disposition", content_disp.encode("latin-1")
            )


def _guess_media_type(path: str) -> str:
    """Guess media type from file extension."""
    media_type, _ = mimetypes.guess_type(path)
    return media_type or "application/octet-stream"
