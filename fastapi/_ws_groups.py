"""WebSocket connection groups for efficient broadcast.

Usage:
    from fastapi._ws_groups import WsConnectionGroup

    groups = WsConnectionGroup()

    # In endpoint:
    async def ws_endpoint(websocket):
        groups.join(websocket, "chat-room-1")
        try:
            async for msg in websocket.iter_text():
                groups.broadcast_text("chat-room-1", msg)
        finally:
            groups.leave(websocket, "chat-room-1")
"""

from __future__ import annotations

from collections import defaultdict
from typing import Any
from weakref import WeakSet

try:
    from fastapi._fastapi_core import (
        ws_build_frame_bytes as _ws_build_frame_bytes,
        ws_serialize_json as _ws_serialize_json,
    )
except ImportError:
    _ws_build_frame_bytes = _ws_serialize_json = None


class WsConnectionGroup:
    """Lightweight connection groups for WebSocket broadcast.

    Frame bytes are built once and written to all transports in the group.
    Since server→client frames are unmasked, the same bytes can be shared.
    Uses WeakSet to auto-cleanup disconnected connections.
    """

    __slots__ = ('_groups',)

    def __init__(self):
        self._groups: dict[str, WeakSet] = defaultdict(WeakSet)

    def join(self, ws, group: str) -> None:
        """Add a WebSocket connection to a group."""
        self._groups[group].add(ws)

    def leave(self, ws, group: str) -> None:
        """Remove a WebSocket connection from a group."""
        s = self._groups.get(group)
        if s:
            s.discard(ws)
            if not s:
                del self._groups[group]

    def leave_all(self, ws) -> None:
        """Remove a WebSocket connection from all groups."""
        empty = []
        for name, s in self._groups.items():
            s.discard(ws)
            if not s:
                empty.append(name)
        for name in empty:
            del self._groups[name]

    def broadcast_text(self, group: str, data: str) -> int:
        """Send text to all connections in group. Returns count sent."""
        s = self._groups.get(group)
        if not s:
            return 0
        payload = data.encode('utf-8') if isinstance(data, str) else data
        if _ws_build_frame_bytes is not None:
            frame = _ws_build_frame_bytes(0x1, payload)
        else:
            from fastapi._cpp_server import CppWebSocket
            frame = CppWebSocket._build_frame_py(0x1, payload)
        count = 0
        for ws in list(s):
            if not ws._closed:
                try:
                    ws._write_frame(frame)
                    count += 1
                except Exception:
                    pass
        return count

    def broadcast_bytes(self, group: str, data: bytes) -> int:
        """Send binary data to all connections in group. Returns count sent."""
        s = self._groups.get(group)
        if not s:
            return 0
        if _ws_build_frame_bytes is not None:
            frame = _ws_build_frame_bytes(0x2, data)
        else:
            from fastapi._cpp_server import CppWebSocket
            frame = CppWebSocket._build_frame_py(0x2, data)
        count = 0
        for ws in list(s):
            if not ws._closed:
                try:
                    ws._write_frame(frame)
                    count += 1
                except Exception:
                    pass
        return count

    def broadcast_json(self, group: str, data: Any) -> int:
        """Send JSON to all connections in group. Returns count sent."""
        if _ws_serialize_json is not None:
            json_bytes = _ws_serialize_json(data)
        else:
            import json
            json_bytes = json.dumps(data).encode('utf-8')
        return self.broadcast_text(group, json_bytes)

    def members(self, group: str) -> int:
        """Return count of connections in a group."""
        s = self._groups.get(group)
        return len(s) if s else 0

    def groups_for(self, ws) -> list[str]:
        """Return list of groups a connection belongs to."""
        return [name for name, s in self._groups.items() if ws in s]
