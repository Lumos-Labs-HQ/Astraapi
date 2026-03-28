# AstraAPI Release Notes

## 0.1.0 (2026-03-29)

### Initial Release

First public release of **AstraAPI** — a high-performance Python web framework powered by a C++ core.

### Features

- **C++ core engine** (`_astraapi_core.so`) — trie-based router, JSON parsing via yyjson, HTTP parsing via llhttp, float serialization via ryu
- **Zero Starlette dependency** — all routing, request, response, middleware classes reimplemented natively
- **Lazy imports** — `from astraapi import AstraAPI` starts in ~1s instead of ~4.7s
- **Full FastAPI-compatible API** — `AstraAPI`, `APIRouter`, `Depends`, `Query`, `Path`, `Body`, `Header`, `Cookie`, `Form`, `File`, `Security`
- **Pydantic v2** request/response validation
- **OpenAPI / Swagger UI** auto-generation
- **WebSocket** support with C++ frame parser
- **Dependency injection** with yield, context managers, scopes
- **Middleware** — CORS, GZip, HTTPS redirect, Trusted Host, WSGI
- **Background tasks**, **lifespan** events
- **Multi-worker** support
- **Optional compression** — libdeflate (preferred) or zlib; brotli if available
- Supports Python 3.9 – 3.14
