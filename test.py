import asyncio
import sys

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, BackgroundTasks
from fastapi._ws_groups import WsConnectionGroup

app = FastAPI()

@app.get("/")
def root():
    return {"message": "Hello World"}

@app.get("/async")
async def root():
    return {"message": "Hello World"}


# Use WsConnectionGroup: builds frame once, broadcasts to all — O(1) frame build
# regardless of room size. WeakSet auto-cleans disconnected connections.
_groups = WsConnectionGroup()


async def broadcast_message(room_id: str, message: str):
    await asyncio.sleep(2)  # simulate heavy work
    _groups.broadcast_text(room_id, f"[background] {message}")


@app.post("/send/{room_id}")
async def send_message(room_id: str, background_tasks: BackgroundTasks):
    background_tasks.add_task(broadcast_message, room_id, "Hello from background task")
    return {"status": "task started"}

@app.websocket("/ws/{room_id}")
async def websocket_room(websocket: WebSocket, room_id: str):
    await websocket.accept()

    _groups.join(websocket, room_id)

    try:
        while True:
            data = await websocket.receive_text()
            _groups.broadcast_text(room_id, data)

    except WebSocketDisconnect:
        _groups.leave(websocket, room_id)


if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8002
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    
    print(f"🚀 Starting server at {host}:{port}...")
    app.run(host=host, port=port, workers=2)
    
