"""
Raw Starlette app with identical endpoints.
This is the baseline — no FastAPI, no C++, just pure Starlette + uvicorn.
"""

from starlette.applications import Starlette
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.routing import Route
import json


async def root(request: Request) -> JSONResponse:
    return JSONResponse({"message": "Hello World"})


async def simple(request: Request) -> JSONResponse:
    return JSONResponse({"status": "ok", "data": [1, 2, 3, 4, 5]})


async def get_item(request: Request) -> JSONResponse:
    item_id = int(request.path_params["item_id"])
    return JSONResponse({"item_id": item_id, "name": f"Item {item_id}", "price": 99.99})


async def search(request: Request) -> JSONResponse:
    q = request.query_params.get("q", "")
    page = int(request.query_params.get("page", "1"))
    limit = int(request.query_params.get("limit", "10"))
    sort = request.query_params.get("sort", "name")
    return JSONResponse({
        "query": q,
        "page": page,
        "limit": limit,
        "sort": sort,
        "results": []
    })


async def create_item(request: Request) -> JSONResponse:
    body = await request.json()
    return JSONResponse({"id": 1, **body})


async def create_user(request: Request) -> JSONResponse:
    body = await request.json()
    return JSONResponse({"created": True, "user": body})


async def large_response(request: Request) -> JSONResponse:
    count = int(request.query_params.get("count", "100"))
    return JSONResponse({
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
    })


async def read_headers(request: Request) -> JSONResponse:
    return JSONResponse({
        "user_agent": request.headers.get("user-agent"),
        "accept": request.headers.get("accept"),
        "authorization": request.headers.get("authorization"),
    })


async def read_cookies(request: Request) -> JSONResponse:
    return JSONResponse({
        "session_id": request.cookies.get("session_id"),
        "theme": request.cookies.get("theme"),
    })


app = Starlette(routes=[
    Route("/", root),
    Route("/simple", simple),
    Route("/items/{item_id:int}", get_item),
    Route("/search", search),
    Route("/items", create_item, methods=["POST"]),
    Route("/users", create_user, methods=["POST"]),
    Route("/large", large_response),
    Route("/headers", read_headers),
    Route("/cookies", read_cookies),
])
