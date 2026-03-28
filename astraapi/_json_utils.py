"""Fast JSON utilities — uses orjson when available, falls back to stdlib json.

orjson is ~5-10x faster than stdlib json and returns bytes directly.
"""
from __future__ import annotations

import json as _json
from typing import Any

try:
    import orjson as _orjson

    def json_dumps(obj: Any) -> bytes:
        """Serialize to JSON bytes (compact, no ASCII escaping)."""
        return _orjson.dumps(obj)

    def json_dumps_str(obj: Any) -> str:
        """Serialize to JSON string."""
        return _orjson.dumps(obj).decode("utf-8")

    def json_loads(data: bytes | str) -> Any:
        """Deserialize JSON bytes or string."""
        return _orjson.loads(data)

except ImportError:

    def json_dumps(obj: Any) -> bytes:
        """Serialize to JSON bytes (compact, no ASCII escaping)."""
        return _json.dumps(
            obj, ensure_ascii=False, allow_nan=False,
            indent=None, separators=(",", ":"),
        ).encode("utf-8")

    def json_dumps_str(obj: Any) -> str:
        """Serialize to JSON string."""
        return _json.dumps(
            obj, ensure_ascii=False, allow_nan=False,
            indent=None, separators=(",", ":"),
        )

    def json_loads(data: bytes | str) -> Any:
        """Deserialize JSON bytes or string."""
        return _json.loads(data)
