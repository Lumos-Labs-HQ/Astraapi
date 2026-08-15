"""FastAPI benchmark server — single worker, pinned to 1 core.

Identical endpoints to astraapi_server.py for fair comparison.
Run: taskset -c 1 uvicorn servers.fastapi_server:app --host 127.0.0.1 --port 8002 --workers 1
"""

from fastapi import FastAPI, WebSocket
from fastapi.responses import Response
from pydantic import BaseModel

app = FastAPI()


class Item(BaseModel):
    name: str
    price: float
    description: str | None = None
    tax: float | None = None


class LargePayload(BaseModel):
    title: str
    content: str
    tags: list[str] = []
    metadata: dict[str, str] = {}


class User(BaseModel):
    id: int
    name: str
    email: str
    is_active: bool = True


class NestedResponse(BaseModel):
    users: list[User]
    total: int
    page: int



@app.get("/plaintext")
def plaintext():
    return "Hello, World!"


@app.get("/json")
def json_endpoint():
    return {"message": "Hello, World!"}


@app.get("/db")
def single_query():
    return {
        "id": 1,
        "randomNumber": 4242,
        "message": "database result",
    }


@app.get("/user/{user_id}")
def get_user(user_id: int):
    return {"id": user_id, "name": "John Doe", "email": "john@example.com"}


@app.get("/search")
def search(q: str = "", limit: int = 10, offset: int = 0):
    return {
        "query": q,
        "limit": limit,
        "offset": offset,
        "results": [{"id": i, "title": f"Result {i}"} for i in range(min(limit, 5))],
    }


@app.post("/items")
def create_item(item: Item):
    return {"id": 1, "name": item.name, "price": item.price}


@app.post("/upload")
def upload_large(payload: LargePayload):
    return {"title": payload.title, "size": len(payload.content), "tags": len(payload.tags)}


@app.get("/nested")
def nested_json():
    users = [
        User(id=i, name=f"User {i}", email=f"user{i}@example.com")
        for i in range(20)
    ]
    return NestedResponse(users=users, total=100, page=1)


@app.get("/headers")
def headers_endpoint():
    return Response(
        content=b'{"status":"ok"}',
        media_type="application/json",
        headers={
            "X-Request-Id": "bench-12345",
            "X-RateLimit-Remaining": "99",
            "X-Custom-Header": "benchmark-value",
            "Cache-Control": "no-cache, no-store",
        },
    )


@app.websocket("/ws/echo")
async def ws_echo(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(data)
    except Exception:
        pass


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=8002, workers=1)
