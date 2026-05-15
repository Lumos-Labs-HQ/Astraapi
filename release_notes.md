# AstraAPI 0.2.0 Release Notes

**Release Date:** May 2026  
**GitHub:** https://github.com/Lumos-Labs-HQ/Astraapi  
**PyPI:** https://pypi.org/project/astraapi/0.2.0  
**Python:** 3.12+ | **C++:** 20

---

## Overview

AstraAPI 0.2.0 is the first stable beta release of our FastAPI-compatible web framework powered by a compiled C++20 core. Zero Starlette dependency. Zero ASGI server required. This release brings production-ready performance, built-in multi-process workers, comprehensive documentation, and a fully passing test suite of **3,730 tests**.

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def hello():
    return {"message": "Hello World"}

if __name__ == "__main__":
    app.run(port=8000, workers=4)
```

---

## What's New

### Native C++20 HTTP Server
- **Zero Starlette imports** in production code — `astraapi/exceptions.py` and `astraapi/dependencies/utils.py` now use pure Python/C++ replacements
- **Zero ASGI server required** — the C++ core speaks HTTP/1.1 directly via **llhttp** (same parser as Node.js)
- **yyjson** + SIMD JSON serializer for zero-copy encode/decode
- **libdeflate** for gzip compression (2-3× faster than zlib)
- **Batch keep-alive sweep** instead of per-connection timers
- Protocol object pooling (OPT-14) for connection reuse
- Zero-copy response pipeline for static files and JSON

### Built-in Multi-Process Workers
- **No gunicorn or uvicorn required** — `app.run(workers=N)` handles everything natively
- Fork-based workers on Linux, spawn-based on Windows
- Shared socket with SO_REUSEPORT (Linux) for kernel-level load balancing
- Automatic worker health monitoring and restart
- Zero-downtime graceful shutdown

### FastAPI Compatibility — Without the Dependencies
- Drop-in replacement for `FastAPI` → `AstraAPI`
- Same `@app.get()`, `@app.post()` decorators
- Full Pydantic v2 model validation
- Same OpenAPI / Swagger UI / ReDoc generation
- BackgroundTasks, WebSockets, File Uploads, Form Data
- Dependency injection system (`Depends()`)
- HTTPException, RequestValidationError handling
- **ASGI middleware compatibility layer** for custom middleware (~99% compatible)

### Test Client
- Real HTTP server backend (not ASGI-in-memory)
- Per-app server sharing with automatic lifecycle management
- Connection pooling via httpx
- WebSocket test session support

### CORS & Security
- Native CORS implementation in C++ core (no middleware overhead)
- CORSMiddleware for advanced configurations
- Security schemes: HTTPBearer, HTTPBasic, OAuth2, API Key

### Documentation Site
- Complete VitePress documentation site (40+ pages)
- Architecture deep-dives (llhttp, yyjson, memory model, zero-copy)
- Feature guides (routing, validation, file uploads, WebSockets)
- Performance benchmarks and optimization guides
- Deployment guides (Docker, production checklist, workers)
- Code examples (CRUD API, hello world, realtime chat)

---

## Performance

| Metric | FastAPI + Uvicorn | AstraAPI 0.2.0 |
|--------|-------------------|----------------|
| HTTP Parser | httptools (Python) | **llhttp (C++)** |
| JSON Engine | orjson | **yyjson + SIMD** |
| Server | uvicorn/gunicorn | **Native C++** |
| Workers | external process manager | **Built-in** |
| Keep-Alive | Per-connection timers | **Batch sweep** |
| Compression | zlib | **libdeflate** |
| Throughput | ~45k req/s | **~236k req/s** |
| Latency p99 | ~2.5ms | **~0.8ms** |

*Benchmarks: Python 3.12, C++20, AMD Ryzen 9, 16 workers, wrk2*

---

## Zero-Dependency Philosophy

AstraAPI removes the entire Starlette/ASGI server stack from the hot path:

| Layer | FastAPI | AstraAPI |
|-------|---------|----------|
| HTTP Parser | Starlette → httptools | **C++ llhttp** |
| Routing | Starlette → Python dict | **C++ trie** |
| JSON | orjson | **C++ yyjson** |
| Server | uvicorn → asyncio | **C++ epoll/kqueue/IOCP** |
| Workers | gunicorn | **Built-in fork/spawn** |
| Exceptions | Starlette HTTPException | **Native Python** |
| Datastructures | Starlette Headers/QueryParams | **Native Python** |

**What this means:**
- `pip install astraapi` installs **only** `pydantic`, `typing-extensions`, `annotated-doc`, `uvloop`/`winloop`, `watchfiles`, and `orjson`
- No Starlette in your dependency tree
- No uvicorn/gunicorn in your dependency tree
- The C++ extension is built once at install time via scikit-build-core

---

## Bug Fixes

### Test Stability
- **Fixed flaky custom route tests** (`test_gzip_request`) caused by two race conditions:
  1. TestClient server reuse when Python's allocator recycled `id(app)` — now uses monotonic `_app_instance_id`
  2. ContextVar race in `_asgi_shim` dispatch when multiple connections were active — C++ core now injects `__raw_headers__`, `__method__`, `__path__` directly into kwargs for custom route classes
- Full test suite now passes reliably: **3,730 passed, 4 skipped, 9 xfailed**

### Memory & Threading
- Fixed memory leaks in C++ route registration and protocol pooling
- Fixed thread leak issue in TestClient shared server registry
- Fixed extra overhead in async dependency resolution
- Fixed context propagation issues between server and test threads

### WebSocket
- Fixed WebSocket test case stability
- Improved WebSocket frame handling and close handshake

### UploadFile / Form Data
- Changed UploadFile implementation in C++ core for better memory efficiency
- Fixed multipart form parsing edge cases

---

## Architecture

```
┌─────────────────────────────────────────┐
│           Python Layer                  │
│  (AstraAPI, decorators, Pydantic,       │
│   OpenAPI, dependencies)                │
├─────────────────────────────────────────┤
│         C++ Core Bridge                 │
│  (pybind11, GIL management)             │
├─────────────────────────────────────────┤
│           C++ Core                      │
│  llhttp → Router → yyjson → Response    │
│  libdeflate | Buffer Pool | Protocol    │
│  epoll/kqueue/IOCP | Worker Forking     │
└─────────────────────────────────────────┘
```

### Key Design Decisions
- **Native HTTP server:** The C++ core opens its own listening socket and handles HTTP/1.1 directly — no uvicorn, no gunicorn, no ASGI server required
- **Zero Starlette in production:** All Starlette imports removed from production code. Exceptions, datastructures, routing, and request handling are native AstraAPI implementations
- **ASGI compatibility layer:** Custom middleware that speaks ASGI is supported via a compatibility shim, but the hot path bypasses ASGI entirely
- **Lazy imports:** `from astraapi import AstraAPI` takes ~1s (vs ~4.7s for FastAPI) thanks to deferred Pydantic/OpenAPI loading
- **GC freeze at startup:** Startup objects moved to permanent generation; gen0 stays nearly empty during request processing

---

## Breaking Changes

None from 0.1.x — AstraAPI 0.2.0 is backward-compatible with all 0.1.x APIs.

**Minimum Python version raised to 3.12** (was 3.10 in early 0.1.x builds).

**Starlette exception handlers:** If you registered exception handlers using `starlette.exceptions.HTTPException`, update them to use `astraapi.exceptions.HTTPException` instead. The C++ core still catches Starlette exceptions for backward compatibility, but handler registration should use AstraAPI's native exception class.

---

## Installation

```bash
pip install astraapi
```

Requires Python 3.12+ and a C++20-capable compiler (GCC 10+, Clang 12+, MSVC 2019+). The C++ extension is built automatically during installation via scikit-build-core.

### Platform Support
- ✅ Linux (x86_64, aarch64)
- ✅ macOS (x86_64, Apple Silicon)
- ✅ Windows (x86_64)

---

## Documentation

Full documentation is available at the GitHub repository or built locally:

```bash
cd docs
bun install
bun run docs:build
```

### Doc Structure
- **Guide:** Installation, quickstart, project structure, running servers
- **Architecture:** C++ core, HTTP pipeline, memory model, Python bridge, zero-copy
- **Features:** Routing, validation, request/response, middleware, security, WebSockets, file uploads, streaming, static files
- **Performance:** Benchmarks, optimization tips
- **Examples:** Hello world, CRUD API, realtime chat
- **Testing:** Sync/async tests, database tests
- **Deployment:** Docker, production checklist, workers

---

## Contributors

This release represents the work of the AstraAPI core team and community contributors. Special thanks to everyone who reported issues, contributed fixes, and provided feedback during the 0.1.x development cycle.

---

## Upgrade from 0.1.x

```bash
pip install --upgrade astraapi
```

No code changes required for most users. If you explicitly import from `starlette.exceptions` or `starlette.datastructures` in your AstraAPI apps, switch to the equivalent `astraapi` imports:

```python
# Before
from starlette.exceptions import HTTPException
from starlette.datastructures import QueryParams

# After
from astraapi import HTTPException
from astraapi.datastructures import QueryParams
```

---

## Known Issues

- WebSocket compression (permessage-deflate) is not yet supported
- HTTP/2 is planned for 0.3.0
- ASGI middleware compatibility is at ~99%; edge cases with streaming middleware may fall back to Python path

---

## Roadmap

### 0.3.0 (Planned)
- HTTP/2 support
- WebSocket compression
- HTTP/3 (QUIC) experimental
- ASGI middleware 100% compatibility


---

## License

MIT License — see [LICENSE](https://github.com/Lumos-Labs-HQ/Astraapi/blob/main/LICENSE) for details.
