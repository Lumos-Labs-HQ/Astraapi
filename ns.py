"""
WebSocket Chat Server — full-featured chat over the C++ HTTP server.
Tests: broadcast, user tracking, online list, typing indicators, timestamps.
"""

import json
import time
from fastapi import FastAPI, WebSocket, WebSocketDisconnect

app = FastAPI()


# ── Connection manager ────────────────────────────────────────────────────────

class ConnectionManager:
    def __init__(self):
        self.connections: dict[str, WebSocket] = {}  # username → ws
        self._msg_id = 0

    def _next_id(self) -> int:
        self._msg_id += 1
        return self._msg_id

    async def connect(self, username: str, ws: WebSocket):
        await ws.accept()
        self.connections[username] = ws
        print(f"[+] {username} connected  ({len(self.connections)} online)")
        # Tell the new user who's already here
        await ws.send_text(json.dumps({
            "type": "system",
            "message": f"Welcome, {username}! You are connected.",
            "id": self._next_id(),
            "timestamp": time.time(),
        }))
        # Broadcast join + updated user list
        await self.broadcast({
            "type": "join",
            "username": username,
            "message": f"{username} joined the chat",
            "id": self._next_id(),
            "timestamp": time.time(),
        })
        await self.broadcast_user_list()

    def disconnect(self, username: str):
        self.connections.pop(username, None)
        print(f"[-] {username} disconnected  ({len(self.connections)} online)")

    async def broadcast(self, message: dict, exclude: str | None = None):
        """Send to all connected clients (optionally excluding one)."""
        data = json.dumps(message)
        dead = []
        for uname, ws in self.connections.items():
            if uname == exclude:
                continue
            try:
                await ws.send_text(data)
            except Exception:
                dead.append(uname)
        for uname in dead:
            self.connections.pop(uname, None)

    async def send_to(self, username: str, message: dict):
        """Send a private message to one user."""
        ws = self.connections.get(username)
        if ws:
            try:
                await ws.send_text(json.dumps(message))
            except Exception:
                self.connections.pop(username, None)

    async def broadcast_user_list(self):
        await self.broadcast({
            "type": "users",
            "users": sorted(self.connections.keys()),
            "count": len(self.connections),
            "timestamp": time.time(),
        })


manager = ConnectionManager()


# ── Serve index.html at / ─────────────────────────────────────────────────────

@app.get("/")
async def root():
    import os
    html_path = os.path.join(os.path.dirname(__file__) or ".", "index.html")
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()
    from starlette.responses import HTMLResponse
    return HTMLResponse(html)


# ── WebSocket chat endpoint ───────────────────────────────────────────────────

@app.websocket("/ws/{username}")
async def websocket_chat(websocket: WebSocket, username: str):
    # Reject duplicate usernames
    if username in manager.connections:
        await websocket.accept()
        await websocket.send_text(json.dumps({
            "type": "error",
            "message": f"Username '{username}' is already taken.",
        }))
        await websocket.close(1008)
        return

    await manager.connect(username, websocket)

    try:
        while True:
            raw = await websocket.receive_text()
            data = json.loads(raw)
            msg_type = data.get("type", "message")

            if msg_type == "message":
                # Broadcast chat message to everyone
                await manager.broadcast({
                    "type": "message",
                    "username": username,
                    "message": data.get("message", ""),
                    "id": manager._next_id(),
                    "timestamp": time.time(),
                })

            elif msg_type == "typing":
                # Broadcast typing indicator to others
                await manager.broadcast({
                    "type": "typing",
                    "username": username,
                }, exclude=username)

            elif msg_type == "stop_typing":
                await manager.broadcast({
                    "type": "stop_typing",
                    "username": username,
                }, exclude=username)

            elif msg_type == "pm":
                # Private message: {"type":"pm", "to":"bob", "message":"hi"}
                target = data.get("to", "")
                if target in manager.connections:
                    pm = {
                        "type": "pm",
                        "from": username,
                        "message": data.get("message", ""),
                        "id": manager._next_id(),
                        "timestamp": time.time(),
                    }
                    await manager.send_to(target, pm)
                    await manager.send_to(username, pm)  # echo to sender
                else:
                    await manager.send_to(username, {
                        "type": "error",
                        "message": f"User '{target}' is not online.",
                    })

            elif msg_type == "ping":
                await websocket.send_text(json.dumps({
                    "type": "pong",
                    "timestamp": time.time(),
                    "server_time": time.time(),
                }))

    except WebSocketDisconnect:
        pass
    except Exception as e:
        print(f"[!] Error for {username}: {e}")
    finally:
        manager.disconnect(username)
        await manager.broadcast({
            "type": "leave",
            "username": username,
            "message": f"{username} left the chat",
            "id": manager._next_id(),
            "timestamp": time.time(),
        })
        await manager.broadcast_user_list()


if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8000
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    app.run(host=host, port=port)
