# WebSockets

AstraAPI supports WebSockets with a C++ fast path that includes direct FD `writev()`, SIMD unmasking, and ring buffer pooling.

## Basic WebSocket

```python
from astraapi import AstraAPI, WebSocket

app = AstraAPI()

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_text()
        await websocket.send_text(f"Message text was: {data}")
```

## WebSocket with Path Params

```python
@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: int):
    await websocket.accept()
    await websocket.send_text(f"Hello client #{client_id}")
    while True:
        data = await websocket.receive_text()
        await websocket.send_text(f"Client {client_id} sent: {data}")
```

## Binary Messages

```python
@app.websocket("/ws/binary")
async def websocket_binary(websocket: WebSocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_bytes()
        await websocket.send_bytes(b"Echo: " + data)
```

## JSON Messages

```python
@app.websocket("/ws/json")
async def websocket_json(websocket: WebSocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_json()
        await websocket.send_json({"echo": data})
```

## Connection Manager

```python
class ConnectionManager:
    def __init__(self):
        self.active_connections: list[WebSocket] = []
    
    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)
    
    def disconnect(self, websocket: WebSocket):
        self.active_connections.remove(websocket)
    
    async def send_personal_message(self, message: str, websocket: WebSocket):
        await websocket.send_text(message)
    
    async def broadcast(self, message: str):
        for connection in self.active_connections:
            await connection.send_text(message)

manager = ConnectionManager()

@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: int):
    await manager.connect(websocket)
    try:
        while True:
            data = await websocket.receive_text()
            await manager.broadcast(f"Client #{client_id} says: {data}")
    except WebSocketDisconnect:
        manager.disconnect(websocket)
```

## WebSocket with Query Params

```python
@app.websocket("/ws")
async def websocket_endpoint(
    websocket: WebSocket,
    token: str | None = None,
):
    if token != "expected_token":
        await websocket.close(code=1008)
        return
    
    await websocket.accept()
    # ...
```

## Dependencies with WebSockets

```python
async def get_ws_user(websocket: WebSocket):
    token = websocket.query_params.get("token")
    user = await verify_token(token)
    if not user:
        await websocket.close(code=1008)
        raise WebSocketException(code=1008, reason="Invalid token")
    return user

@app.websocket("/ws")
async def websocket_endpoint(
    websocket: WebSocket,
    user: Annotated[User, Depends(get_ws_user)],
):
    await websocket.accept()
    await websocket.send_text(f"Hello {user.name}!")
```

## WebSocket Events

```python
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            message = await websocket.receive()
            if message["type"] == "websocket.receive":
                if "text" in message:
                    await websocket.send_text(f"Echo: {message['text']}")
                elif "bytes" in message:
                    await websocket.send_bytes(message["bytes"])
            elif message["type"] == "websocket.disconnect":
                break
    except WebSocketDisconnect:
        print("Client disconnected")
```

## Close Codes

| Code | Name | Description |
|------|------|-------------|
| 1000 | NORMAL | Regular closure |
| 1001 | GOING_AWAY | Server shutting down |
| 1008 | POLICY_VIOLATION | Invalid auth, etc. |
| 1011 | INTERNAL_ERROR | Server error |

```python
await websocket.close(code=1000, reason="Goodbye")
```

## C++ Fast Path

AstraAPI's WebSocket implementation has a C++ fast path that bypasses Python's transport entirely:

- **Direct FD `writev()`** — builds `iovec` pairs with zero payload copy from the ring buffer
- **SIMD unmasking** — AVX2 (32 bytes), SSE2 (16 bytes), NEON (ARM), or scalar fallback
- **Ring buffer pooling** — `_WsConnectionPool` reuses WebSocket ring buffer capsules
- **`TCP_CORK`** — batches TCP segments during WebSocket write bursts (Linux)

## Performance Note

WebSocket connections in AstraAPI use the same protocol object pool as HTTP connections. The C++ core handles WebSocket frame parsing, while Python manages the application logic. At scale, WebSocket connections are lightweight — each connection uses ~12KB of memory.
