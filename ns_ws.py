import asyncio

from astraapi import AstraAPI, WebSocket, Header, HTTPException
from astraapi.middleware.cors import CORSMiddleware
from astraapi.responses import StreamingResponse, EventSourceResponse, ServerSentEvent
from astraapi import UploadFile

app = AstraAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/")
def root():
    return {"message": "Hello World"}

@app.get("/async")
async def async_root():
    return {"message": "Hello World"}


async def generate_sse():
    for i in range(5):
        yield ServerSentEvent(data={"step": i, "message": f"Processing {i}"}, event="progress")
        await asyncio.sleep(1)
    yield ServerSentEvent(data={"done": True}, event="complete")


@app.get("/events")
async def events():
    return EventSourceResponse(generate_sse())


@app.get("/protected")
async def protected_endpoint(token: str = Header()):
    if token != "123":
        raise HTTPException(status_code=401, detail="Unauthorized")
    return {"message": "You are authorized!"}


@app.post("/upload")
async def upload(file: UploadFile):
    filename = file.filename or "uploaded_file"
    contents = await file.read()
    with open(filename, "wb") as f:
        f.write(contents)
    return {"message": f"File '{filename}' uploaded successfully"}


async def generate_story():

    story = [
        "Once upon a time, ",
        "there was a developer named Rana. ",
        "He was learning FastAPI streaming. ",
        "Then he built an awesome realtime app 🚀"
        "The end."
        "credits: jack"
        "p.s. this is a demo story, not a real one."
        "Hope you enjoyed it!"
        "The moral of the story is: keep learning and building cool stuff!"
    ]

    for chunk in story:
        yield chunk
        await asyncio.sleep(1)


@app.get("/story")
async def story():
    return StreamingResponse(
        generate_story(),
        media_type="text/plain"
    )

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