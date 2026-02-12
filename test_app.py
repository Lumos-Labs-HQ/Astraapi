"""
FastAPI app with C++ core acceleration enabled.
Used for benchmarking against pure Python FastAPI.
Comprehensive test app: all features (CORS, DI, response_model, WebSocket, etc.)
"""

from fastapi import FastAPI, Query, Header, Cookie, Body, Depends, WebSocket, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from starlette.websockets import WebSocketDisconnect
from pydantic import BaseModel
from typing import Optional

app = FastAPI(title="FastAPI + Core")

# ── CORS middleware ──────────────────────────────────────────────────────────
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:3000", "https://example.com"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ── Models ───────────────────────────────────────────────────────────────────

class Item(BaseModel):
    name: str
    price: float
    quantity: int = 1
    tags: list[str] = []


class User(BaseModel):
    username: str
    email: str
    age: Optional[int] = None


class LargePayload(BaseModel):
    user_id: int
    username: str
    email: str
    first_name: str
    last_name: str
    age: int
    address: dict
    metadata: dict
    tags: list[str]
    is_active: bool
    created_at: str
    updated_at: str


class UserResponse(BaseModel):
    id: int
    username: str
    email: str
    is_active: bool = True


# ── Dependencies ─────────────────────────────────────────────────────────────

async def common_params(q: str = Query(default=""), skip: int = Query(default=0)):
    return {"q": q, "skip": skip}


# ── Original endpoints ───────────────────────────────────────────────────────

@app.get("/")
async def root():
    return {"message": "Hello World"}


@app.get("/simple")
async def simple():
    return {"status": "ok", "data": [1, 2, 3, 4, 5]}


@app.get("/items/{item_id}")
async def get_item(item_id: int):
    return {"item_id": item_id, "name": f"Item {item_id}", "price": 99.99}


@app.get("/search")
async def search(
    q: str = Query(...),
    page: int = Query(default=1, ge=1),
    limit: int = Query(default=10, ge=1, le=100),
    sort: str = Query(default="name"),
):
    return {
        "query": q,
        "page": page,
        "limit": limit,
        "sort": sort,
        "results": []
    }


@app.post("/items")
async def create_item(item: Item):
    return {"id": 1, **item.model_dump()}


@app.post("/users")
async def create_user(user: User):
    return {"created": True, "user": user.model_dump()}


@app.get("/large")
async def large_response(count: int = Query(default=100, ge=1, le=1000)):
    return {
        "items": [
            {
                "id": i,
                "name": f"Item {i}",
                "price": round(9.99 + i * 0.5, 2),
                "active": i % 2 == 0,
                "tags": ["tag1", "tag2"] if i % 3 == 0 else ["tag3"],
            }
            for i in range(count)
        ]
    }


@app.get("/headers")
async def read_headers(
    user_agent: Optional[str] = Header(default=None),
    accept: Optional[str] = Header(default=None),
    authorization: Optional[str] = Header(default=None),
):
    return {
        "user_agent": user_agent,
        "accept": accept,
        "authorization": authorization,
    }


@app.get("/cookies")
async def read_cookies(
    session_id: Optional[str] = Cookie(default=None),
    theme: Optional[str] = Cookie(default=None),
):
    return {"session_id": session_id, "theme": theme}


# ── New benchmark endpoints ──────────────────────────────────────────────────

@app.post("/post_raw")
async def post_raw(body: dict = Body(...)):
    return body


@app.post("/post_large")
async def post_large(payload: LargePayload):
    return {"success": True, "user_id": payload.user_id}


@app.get("/mixed/{user_id}")
async def mixed_params(
    user_id: int,
    q: str = Query(...),
    limit: int = Query(default=10),
    authorization: Optional[str] = Header(default=None),
    session_id: Optional[str] = Cookie(default=None),
):
    return {
        "user_id": user_id,
        "query": q,
        "limit": limit,
        "has_auth": authorization is not None,
        "has_session": session_id is not None,
    }


@app.get("/with_deps")
async def with_dependencies(commons: dict = Depends(common_params)):
    return {"result": "ok", **commons}


@app.get("/user/{user_id}", response_model=UserResponse)
async def get_user_validated(user_id: int):
    return {
        "id": user_id,
        "username": f"user{user_id}",
        "email": f"user{user_id}@example.com",
        "is_active": True,
        "extra_field": "this should be filtered out",
    }


@app.get("/error_test")
async def error_test(code: int = Query(default=500)):
    raise HTTPException(status_code=code, detail=f"Test error {code}")


@app.post("/validate_error")
async def validate_error(item: Item):
    return {"id": 1, **item.model_dump()}


# ── WebSocket ────────────────────────────────────────────────────────────────

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    data = await websocket.receive_text()
    await websocket.send_text(f"Echo: {data}")
    await websocket.close()


# ── WebSocket benchmark endpoints ───────────────────────────────────────────

@app.websocket("/ws_echo")
async def ws_echo(websocket: WebSocket):
    """Sustained echo — receives text, sends it back, repeat until close."""
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(data)
    except WebSocketDisconnect:
        pass


@app.websocket("/ws_json")
async def ws_json_echo(websocket: WebSocket):
    """JSON echo — receives JSON, sends it back, repeat until close."""
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_json()
            await websocket.send_json(data)
    except WebSocketDisconnect:
        pass


@app.websocket("/ws_throughput")
async def ws_throughput(websocket: WebSocket):
    """Throughput test — client sends count, server pushes that many messages."""
    await websocket.accept()
    msg = await websocket.receive_text()
    count = int(msg) if msg.isdigit() else 1000
    for i in range(count):
        await websocket.send_text(f"msg_{i}")
    await websocket.close()


# ── Debug ────────────────────────────────────────────────────────────────────

@app.get("/debug")
async def debug_info():
    """Show whether core backend is active and which fast-path routes exist."""
    core_active = False
    fast_routes = []
    try:
        from fastapi._core_bridge import CoreApp
        core_active = True
        if hasattr(app, '_core_asgi'):
            fast_routes = sorted(getattr(app._core_asgi, '_fast_routes', []))
    except (ImportError, AttributeError):
        pass
    return {
        "core_active": core_active,
        "fast_path_routes": fast_routes,
        "total_routes": app._core_app.route_count() if hasattr(app, '_core_app') else 0,
    }


if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8002
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    app.run(host=host, port=port)
