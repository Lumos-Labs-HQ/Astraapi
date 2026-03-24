from fastapi import FastAPI, WebSocket

app = FastAPI()

@app.get("/")
def root():
    return {"message": "Hello World"}

@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(data)
    except Exception:
        pass

app.run(host="127.0.0.1", port=8002, workers=1)
