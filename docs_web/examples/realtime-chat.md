# Real-time Chat

A real-time chat server using WebSockets with room support.

## Code

```python
import asyncio
from astraapi import AstraAPI, WebSocket, WebSocketDisconnect
from typing import Dict, List

app = AstraAPI(title="Real-time Chat")

class Room:
    def __init__(self, name: str):
        self.name = name
        self.clients: List[WebSocket] = []
        self.history: List[dict] = []
    
    async def join(self, websocket: WebSocket):
        await websocket.accept()
        self.clients.append(websocket)
        for msg in self.history[-50:]:
            await websocket.send_json(msg)
    
    def leave(self, websocket: WebSocket):
        if websocket in self.clients:
            self.clients.remove(websocket)
    
    async def broadcast(self, message: dict):
        self.history.append(message)
        if len(self.history) > 1000:
            self.history = self.history[-500:]
        
        disconnected = []
        for client in self.clients:
            try:
                await client.send_json(message)
            except Exception:
                disconnected.append(client)
        
        for client in disconnected:
            self.leave(client)

rooms: Dict[str, Room] = {}

def get_room(name: str) -> Room:
    if name not in rooms:
        rooms[name] = Room(name)
    return rooms[name]

@app.websocket("/ws/{room_name}")
async def websocket_endpoint(websocket: WebSocket, room_name: str):
    room = get_room(room_name)
    await room.join(websocket)
    
    try:
        while True:
            data = await websocket.receive_json()
            message = {
                "type": "message",
                "room": room_name,
                "text": data.get("text", ""),
                "user": data.get("user", "Anonymous"),
            }
            await room.broadcast(message)
    except WebSocketDisconnect:
        room.leave(websocket)
        await room.broadcast({
            "type": "system",
            "text": f"A user left {room_name}",
        })

@app.get("/rooms/")
def list_rooms():
    return [
        {"name": name, "clients": len(room.clients)}
        for name, room in rooms.items()
    ]

@app.get("/rooms/{room_name}/history")
def get_history(room_name: str):
    room = rooms.get(room_name)
    if not room:
        raise HTTPException(status_code=404, detail="Room not found")
    return room.history

if __name__ == "__main__":
    app.run(port=8000)
```

## HTML Client

```html
<!DOCTYPE html>
<html>
<head>
    <title>AstraAPI Chat</title>
    <style>
        body { font-family: sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        #messages { border: 1px solid #ccc; height: 400px; overflow-y: auto; padding: 10px; margin: 10px 0; }
        .msg { padding: 5px; margin: 2px 0; background: #f5f5f5; }
        .system { color: #666; font-style: italic; }
        input { padding: 10px; font-size: 16px; }
        button { padding: 10px 20px; font-size: 16px; }
    </style>
</head>
<body>
    <h1>Chat Room: <span id="room">general</span></h1>
    <div id="messages"></div>
    <input type="text" id="user" placeholder="Your name" value="Guest">
    <input type="text" id="text" placeholder="Message..." style="width: 400px;">
    <button onclick="send()">Send</button>

    <script>
        const room = new URLSearchParams(window.location.search).get('room') || 'general';
        document.getElementById('room').textContent = room;
        
        const ws = new WebSocket(`ws://localhost:8000/ws/${room}`);
        const messages = document.getElementById('messages');
        
        ws.onmessage = (event) => {
            const msg = JSON.parse(event.data);
            const div = document.createElement('div');
            div.className = 'msg' + (msg.type === 'system' ? ' system' : '');
            div.textContent = msg.type === 'message' ? `${msg.user}: ${msg.text}` : msg.text;
            messages.appendChild(div);
            messages.scrollTop = messages.scrollHeight;
        };
        
        function send() {
            const text = document.getElementById('text');
            const user = document.getElementById('user');
            ws.send(JSON.stringify({user: user.value, text: text.value}));
            text.value = '';
        }
        
        document.getElementById('text').addEventListener('keypress', (e) => {
            if (e.key === 'Enter') send();
        });
    </script>
</body>
</html>
```

## Run

```bash
python chat.py
```

Open `http://localhost:8000/static/chat.html` in multiple browser tabs.

## Features

- Multiple chat rooms
- Message history (last 1000 messages)
- System messages for join/leave events
- REST API for room listing and history
