"""
Example FastAPI server using Rust-accelerated internals.

Run:
    uvicorn example_server:app --reload

All Rust acceleration happens transparently — same FastAPI API, just faster.
"""

from fastapi import FastAPI, File, Form, UploadFile, Query, Header, Cookie
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from typing import Optional
import time


app = FastAPI(
    title="FastAPI + Rust Core",
    description="Rust-accelerated FastAPI example server",
    version="0.4.0",
    doc="/docs"
)


# --- Models ---

class Item(BaseModel):
    name: str
    price: float
    in_stock: bool = True
    tags: list[str] = []


class User(BaseModel):
    username: str
    email: str
    age: Optional[int] = None


# --- In-memory store ---
items_db: dict[int, dict] = {
    1: {"name": "Laptop", "price": 999.99, "in_stock": True, "tags": ["electronics"]},
    2: {"name": "Book", "price": 12.50, "in_stock": True, "tags": ["education"]},
    3: {"name": "Headphones", "price": 79.99, "in_stock": False, "tags": ["electronics", "audio"]},
}
next_id = 4


@app.get("/")
async def root():
    """Health check — shows whether Rust acceleration is active."""
    return {
        "status": "ok",
    }


@app.get("/items")
async def list_items(
    page: int = Query(default=1, ge=1),
    limit: int = Query(default=10, ge=1, le=100),
    in_stock: Optional[bool] = Query(default=None),
):
    """List items with pagination and optional filter.

    Exercises: route matching, query parsing, scalar coercion, JSON encoding.
    """
    filtered = list(items_db.values())
    if in_stock is not None:
        filtered = [i for i in filtered if i["in_stock"] == in_stock]

    start = (page - 1) * limit
    end = start + limit
    return {
        "items": filtered[start:end],
        "total": len(filtered),
        "page": page,
        "limit": limit,
    }


@app.get("/items/{item_id}")
async def get_item(item_id: int):
    """Get a single item.

    Exercises: route matching with path params, JSON encoding.
    """
    if item_id not in items_db:
        return JSONResponse(status_code=404, content={"detail": "Item not found"})
    return items_db[item_id]


@app.post("/items", status_code=201)
async def create_item(item: Item):
    """Create an item from JSON body.

    Exercises: JSON body parsing (simd-json), Pydantic validation, JSON encoding.
    """
    global next_id
    item_dict = item.model_dump()
    items_db[next_id] = item_dict
    result = {"id": next_id, **item_dict}
    next_id += 1
    return result


@app.put("/items/{item_id}")
async def update_item(item_id: int, item: Item):
    """Update an item.

    Exercises: path params, JSON body parsing, JSON encoding.
    """
    if item_id not in items_db:
        return JSONResponse(status_code=404, content={"detail": "Item not found"})
    items_db[item_id] = item.model_dump()
    return {"id": item_id, **items_db[item_id]}


@app.delete("/items/{item_id}")
async def delete_item(item_id: int):
    """Delete an item."""
    if item_id not in items_db:
        return JSONResponse(status_code=404, content={"detail": "Item not found"})
    del items_db[item_id]
    return {"deleted": True}


@app.post("/users")
async def create_user(user: User):
    """Create a user from JSON body.

    Exercises: JSON body parsing, Pydantic validation with optional fields.
    """
    return {"created": True, "user": user.model_dump()}


@app.post("/upload")
async def upload_file(
    file: UploadFile = File(...),
    description: str = Form(""),
):
    """Upload a file with form data.

    Exercises: multipart form parsing (Rust memchr boundary scan).
    """
    content = await file.read()
    return {
        "filename": file.filename,
        "size": len(content),
        "content_type": file.content_type,
        "description": description,
    }


@app.post("/form")
async def submit_form(
    name: str = Form(...),
    email: str = Form(...),
    age: int = Form(...),
    subscribe: bool = Form(default=False),
):
    """Submit a URL-encoded form.

    Exercises: urlencoded form parsing, scalar coercion.
    """
    return {
        "name": name,
        "email": email,
        "age": age,
        "subscribe": subscribe,
    }


@app.get("/headers")
async def read_headers(
    user_agent: Optional[str] = Header(default=None),
    accept: Optional[str] = Header(default=None),
    x_request_id: Optional[str] = Header(default=None),
):
    """Read request headers.

    Exercises: header parsing and normalization.
    """
    return {
        "user_agent": user_agent,
        "accept": accept,
        "x_request_id": x_request_id,
    }


@app.get("/cookies")
async def read_cookies(
    session_id: Optional[str] = Cookie(default=None),
    theme: Optional[str] = Cookie(default=None),
):
    """Read cookies.

    Exercises: cookie header parsing.
    """
    return {"session_id": session_id, "theme": theme}


@app.get("/search")
async def search(
    q: str = Query(..., min_length=1),
    category: Optional[str] = Query(default=None),
    min_price: Optional[float] = Query(default=None),
    max_price: Optional[float] = Query(default=None),
    in_stock: bool = Query(default=True),
    sort: str = Query(default="name"),
    page: int = Query(default=1, ge=1),
    limit: int = Query(default=20, ge=1, le=100),
):
    """Search endpoint with many query params.

    Exercises: query string parsing (8 params), scalar coercion.
    """
    results = list(items_db.values())
    if q:
        results = [i for i in results if q.lower() in i["name"].lower()]
    if category:
        results = [i for i in results if category in i.get("tags", [])]
    if min_price is not None:
        results = [i for i in results if i["price"] >= min_price]
    if max_price is not None:
        results = [i for i in results if i["price"] <= max_price]
    results = [i for i in results if i["in_stock"] == in_stock]

    return {
        "query": q,
        "results": results,
        "total": len(results),
        "page": page,
    }


@app.get("/large-response")
async def large_response(count: int = Query(default=100, ge=1, le=10000)):
    """Return a large JSON response.

    Exercises: JSON encoding + serialization pipeline for larger payloads.
    """
    return {
        "items": [
            {
                "id": i,
                "name": f"Item {i}",
                "price": round(9.99 + i * 0.5, 2),
                "active": i % 3 != 0,
                "tags": ["tag1", "tag2"] if i % 2 == 0 else ["tag3"],
            }
            for i in range(count)
        ]
    }


@app.post("/validate-error")
async def trigger_validation_error(item: Item):
    """Intentionally use this with bad input to trigger 422 validation errors.

    Exercises: error response serialization (Rust direct → JSON bytes).

    Try: POST with {"name": 123, "price": "not-a-number"}
    """
    return item.model_dump()


@app.get("/timing")
async def timing_test():
    """Simple endpoint to measure raw framework overhead.

    The 'user code' is negligible (~1us), so the response time
    is almost entirely FastAPI framework overhead.
    """
    start = time.perf_counter_ns()
    result = {"data": "hello", "values": [1, 2, 3]}
    elapsed_ns = time.perf_counter_ns() - start
    result["user_code_ns"] = elapsed_ns
    return result


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
