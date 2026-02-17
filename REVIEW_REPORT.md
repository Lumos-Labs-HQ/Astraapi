# FastAPI C++ Core — Complete End-to-End Code Review Report

**Date:** 2026-02-17
**Scope:** Full codebase — 25 C++ source files, 11 headers, 3 third-party C libraries, 40+ Python modules
**Version:** FastAPI 0.128.3 with C++ acceleration layer (`_fastapi_core`)

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Bugs — Must Fix](#2-bugs--must-fix)
3. [Memory Leaks](#3-memory-leaks)
4. [Performance Issues](#4-performance-issues)
5. [Security Concerns](#5-security-concerns)
6. [Starlette Dependency Audit](#6-starlette-dependency-audit)
7. [Middleware Architecture & Gaps](#7-middleware-architecture--gaps)
8. [FastAPI Feature Completeness](#8-fastapi-feature-completeness)
9. [Missing Features in `app.run()` Mode](#9-missing-features-in-apprun-mode)
10. [Thread Safety Concerns](#10-thread-safety-concerns)
11. [Dead / Unused Code](#11-dead--unused-code)
12. [C++ Code Strengths](#12-c-code-strengths)
13. [C++ Replacement Roadmap for Starlette](#13-c-replacement-roadmap-for-starlette)
14. [Priority Action Items](#14-priority-action-items)

---

## 1. Architecture Overview

### Dual Code Paths

```
                         FastAPI.__call__(scope, receive, send)
                                      │
                         ┌────────────┴────────────┐
                         │   Has user middleware?   │
                         └────────────┬────────────┘
                              │              │
                             NO             YES
                              │              │
                    ┌─────────▼──────┐  ┌───▼──────────────────┐
                    │  C++ Fast Path │  │  Starlette Full Path │
                    │  (CoreASGI)    │  │  (middleware chain)  │
                    └─────────┬──────┘  └───┬──────────────────┘
                              │              │
                    C++ does:          Starlette does:
                    - Route match      - ServerErrorMiddleware
                    - CORS check       - CORSMiddleware
                    - Trusted host     - GZipMiddleware
                    - Param extract    - ExceptionMiddleware
                    - JSON serialize   - AsyncExitStackMiddleware
                    - Compression      - Router dispatch
                              │              │
                         Python does:   Python does:
                         - ASGI I/O     - Request object creation
                         - Endpoint     - Dependency injection
                           call         - Full Starlette lifecycle
```

### Third Code Path: `app.run()` (C++ HTTP Server)

```
    app.run(host, port)
           │
    ┌──────▼──────────────────┐
    │  asyncio.Protocol       │
    │  (CppHttpProtocol)      │
    │  - Raw TCP handling     │
    │  - No ASGI at all       │
    │  - No Starlette at all  │
    │  - C++ for everything   │
    └──────┬──────────────────┘
           │
    C++ does ALL:
    - HTTP parsing (llhttp)
    - Route matching (trie)
    - CORS, trusted host
    - Param extraction
    - JSON serialization (yyjson + ryu)
    - Compression (zlib + brotli)
    - WebSocket frame parsing (SIMD)
```

### Critical Architecture Problem

**Adding ANY user middleware disables the C++ ASGI fast path entirely.** When running under uvicorn/hypercorn, if the user adds `CORSMiddleware`, `GZipMiddleware`, or any custom middleware, every request falls back to pure Python/Starlette processing. The C++ optimizations become unused.

### File Inventory

| Category | Count | Key Files |
|----------|-------|-----------|
| C++ headers | 11 | `app.hpp`, `router.hpp`, `ws_frame_parser.hpp`, `ws_ring_buffer.hpp`, `buffer_pool.hpp`, `json_parser.hpp`, `json_writer.hpp`, `http_parser.hpp`, `pyref.hpp`, `asgi_constants.hpp`, `platform.hpp` |
| C++ sources | 25 | `app.cpp`, `router.cpp`, `module.cpp`, `request_parser.cpp`, `response_builder.cpp`, `json_parser.cpp`, `json_writer.cpp`, `json_encoder.cpp`, `http_parser.cpp`, `body_parser.cpp`, `form_parser.cpp`, `param_extractor.cpp`, `middleware_engine.cpp`, `websocket_handler.cpp`, `ws_frame_parser.cpp`, `ws_ring_buffer.cpp`, `dep_engine.cpp`, `dependency_resolver.cpp`, `security.cpp`, `openapi_gen.cpp`, `error_response.cpp`, `response_pipeline.cpp`, `request_pipeline.cpp`, `buffer_pool.cpp`, `utils.cpp`, `asgi_constants.cpp` |
| Third-party C | 3 libs | yyjson (JSON), ryu (float-to-string), llhttp (HTTP parsing) |
| Python bridge | 4 | `_core_bridge.py`, `_core_app.py`, `_cpp_server.py`, `_ws_groups.py` |
| FastAPI modules | 40+ | Full standard FastAPI module set |
| Build config | 3 | `CMakeLists.txt`, `pyproject.toml`, `pdm_build.py` |

---

## 2. Bugs — Must Fix

### BUG-1: Double-free in C++ Exception Handling [CRITICAL]

**File:** `cpp_core/src/app.cpp` lines 1198-1210
**Impact:** Segfault / interpreter crash

```cpp
PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
if (s_http_exc_type && ...) {
    PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
    PyRef detail(...); PyRef sc(...);
    Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb); // DECREF #1
    if (detail && sc) { return build_error_response(...); }
}
Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb); // DECREF #2 = CRASH
```

When the HTTPException branch is entered but `detail && sc` is false, both DECREF blocks execute on already-freed pointers.

**Fix:** Set `exc_type = exc_val = exc_tb = nullptr` after the first DECREF, or wrap in `PyRef`.

---

### BUG-2: Reference Leak in `get_response_filters` [HIGH]

**File:** `cpp_core/src/app.cpp` lines 711-716
**Impact:** Memory leak on every call

```cpp
Py_INCREF(inc);                      // +1
Py_INCREF(exc);                      // +1
return PyTuple_Pack(2, inc, exc);    // PyTuple_Pack also INCREFs = +2 total
```

Each object ends up with one extra reference that is never freed.

**Fix:** Remove the explicit `Py_INCREF` calls — `PyTuple_Pack` already INCREFs.

---

### BUG-3: Cork + `send_json` Double-Framing [HIGH]

**File:** `fastapi/_cpp_server.py` line 474 + line 648
**Impact:** Corrupted WebSocket stream

`_ws_build_json_frame` returns a **complete WebSocket frame** (header + payload). When corking is active, this pre-built frame is appended to `_cork_buf`. `_flush_cork()` then passes it through `_ws_build_frames_batch` which adds **another frame header**, producing invalid double-framed data.

**Fix:** Mark pre-built frames in the cork buffer and skip re-framing in `_flush_cork`.

---

### BUG-4: `_write_frame` Method Missing on `CppWebSocket` [HIGH]

**File:** `fastapi/_ws_groups.py` line 83
**Impact:** WebSocket group broadcast crashes at runtime

```python
ws._write_frame(frame)  # AttributeError — method does not exist!
```

`CppWebSocket` has `_build_frame` (static) and `_queue_send`, but no `_write_frame` method. **All WebSocket group broadcasts are broken.**

**Fix:** Add `_write_frame` to `CppWebSocket` or update `_ws_groups.py` to use the correct method.

---

### BUG-5: WebSocket Scope Missing Critical Fields [MEDIUM]

**File:** `fastapi/_cpp_server.py` lines 325-331
**Impact:** Auth headers, query params invisible to WebSocket endpoints

```python
self.scope = {
    "type": "websocket",
    "path": path,
    "path_params": self.path_params,
    "headers": [],          # ALWAYS EMPTY
    "query_string": b"",    # ALWAYS EMPTY
}
# Missing: client, server, root_path, scheme, subprotocols, state
```

Any dependency or middleware reading headers/query from the WebSocket scope gets empty data. Authentication via headers silently fails.

**Fix:** Parse and populate headers and query_string from the HTTP upgrade request. Add missing scope fields.

---

### BUG-6: WebSocket `accept()` Ignores Subprotocol [MEDIUM]

**File:** `fastapi/_cpp_server.py` lines 335-337
**Impact:** Subprotocol negotiation broken

```python
async def accept(self, subprotocol=None, headers=None) -> None:
    self.application_state = WebSocketState.CONNECTED  # params silently ignored
```

**Fix:** Include `subprotocol` in the WebSocket upgrade response headers.

---

### BUG-7: OpenAPI `status_code` UnboundLocalError [MEDIUM]

**File:** `fastapi/openapi/utils.py` lines 344-357
**Impact:** OpenAPI schema generation crash for certain route configurations

If `route.status_code is None` AND no `status_code` default is found in the response class signature, then `status_code` is never assigned, causing `UnboundLocalError` at line 357.

**Fix:** Add a fallback `status_code = 200` default.

---

### BUG-8: Reference Leak in `py_ws_parse_frames_json` [LOW]

**File:** `cpp_core/src/websocket_handler.cpp` line 192
**Impact:** Small memory leak if `PyTuple_Pack` fails

```cpp
PyRef tuple(PyTuple_Pack(2, PyLong_FromLong(opcode), payload_ref.get()));
```

If `PyTuple_Pack` fails, the `PyLong_FromLong(opcode)` result leaks because it was passed directly without being wrapped in `PyRef` first.

**Fix:** Create `PyRef opcode_obj(PyLong_FromLong(opcode))` first, then pass `.get()` to `PyTuple_Pack`.

---

## 3. Memory Leaks

### LEAK-1: `WsDeflateContext` Has No Destructor

**File:** `cpp_core/src/ws_frame_parser.cpp` lines 958-985

`WsDeflateContext` allocates `z_stream` with `new` but cleanup requires manual `destroy()`. Abandoned contexts leak.

**Fix:** `~WsDeflateContext() { destroy(); }` or use `std::unique_ptr`.

### LEAK-2: Global Registries Not Cleaned on Module Unload

**Files:** `cpp_core/src/param_extractor.cpp`, `cpp_core/src/dep_engine.cpp`

`g_registry` and dependency plan registries hold strong `PyObject*` refs. Module unload without unregistration leaks them.

**Fix:** Add cleanup functions called from `module_free()`.

### LEAK-3: Static Cached Refs Never Freed

**File:** `cpp_core/src/app.cpp` lines 27-33

```cpp
static PyObject* s_http_exc_type = nullptr;
static PyObject* s_resume_func = nullptr;
static PyObject* s_request_body_to_args = nullptr;
static PyObject* g_str_write = nullptr;
static PyObject* g_str_is_closing = nullptr;
```

`cleanup_cached_refs()` is defined but **never called** from `module_free()`.

**Fix:** Register `cleanup_cached_refs()` in the module cleanup path.

### LEAK-4: Unbounded `@lru_cache` on `get_cached_model_fields`

**File:** `fastapi/_compat/v2.py` line 396-398

Cache has no `maxsize`. Apps dynamically creating Pydantic models will accumulate entries forever.

**Fix:** Set `maxsize=1024` or similar reasonable bound.

---

## 4. Performance Issues

### PERF-1: Unbounded HTTP Request Buffer (DoS Vector) [HIGH]

**File:** `fastapi/_cpp_server.py` line 811

```python
buf += data  # No size limit — can grow to exhaustion
```

No maximum buffer size check. A malicious client can exhaust server memory.

**Fix:** Add configurable `max_buffer_size` (default 1MB). Close connections exceeding it.

### PERF-2: WebSocket Backpressure Is Message-Count, Not Byte-Count [HIGH]

**File:** `fastapi/_cpp_server.py` line 54

`_WsFastChannel` pauses at 256 **messages** regardless of size. 255 messages x 64MB max = **16GB potential memory** before backpressure kicks in.

**Fix:** Track byte count in `feed()` and pause based on total bytes (e.g., 64MB).

### PERF-3: ~400 Lines of Duplicated C++ Code [MEDIUM]

**File:** `cpp_core/src/app.cpp` lines 882-1565

`CoreApp_handle_and_respond` and `CoreApp_handle_request_inline` duplicate nearly identical parameter extraction logic. Risk of divergent bugs, increased binary size.

**Fix:** Extract shared param extraction into a helper function.

### PERF-4: `time.monotonic()` Per-WebSocket-Message [LOW]

**Files:** `fastapi/_cpp_server.py` lines 428-429

Each `send_text`/`send_bytes`/`send_json` call invokes `time.monotonic()`. At 100K+ msg/sec, overhead accumulates (~25ns/call x 100K = 2.5ms/sec).

**Fix:** Sample once per event loop iteration or per N messages.

### PERF-5: `_build_field_specs` Called Twice [LOW]

**File:** `fastapi/routing.py` lines 1006-1018

Same function called twice with identical inputs during `APIRoute.__init__`. Result should be cached.

### PERF-6: Security Classes Bypass C++ Extraction [MEDIUM]

**Files:** `fastapi/security/http.py`, `fastapi/security/oauth2.py`, `fastapi/security/api_key.py`

All security classes use `request.headers.get("Authorization")` from Starlette instead of the C++ `extract_bearer_token` / `extract_basic_credentials` functions that already exist in the C++ core.

**Fix:** Wire security classes through the C++ fast path for credential extraction.

### PERF-7: Ring Buffer Wrap-Around Linearization [LOW]

**File:** `cpp_core/src/ws_ring_buffer.cpp` line 140

`readable_contiguous()` does a `memmove` when data wraps around the ring boundary. For sustained high-throughput streams, this causes periodic latency spikes.

### PERF-8: Header Name Truncation at 255 Bytes [LOW]

**File:** `cpp_core/src/app.cpp` lines 1040-1046

Stack buffer `char norm_buf[256]` silently truncates header names > 255 bytes. Rare but causes silent data loss.

---

## 5. Security Concerns

### SEC-1: XSS in OpenAPI Docs HTML Generation [MEDIUM]

**File:** `fastapi/openapi/docs.py` lines 138-155, 250-283

`title` and `openapi_url` are f-string interpolated into HTML without escaping:

```python
html = f"""
<!DOCTYPE html>
<html>
<head>
<title>{title}</title>
...
"""
```

If `title` contains `<script>`, it executes in the browser. Standard usage sets this from developer code, but defense-in-depth dictates escaping.

**Fix:** Use `html.escape()` on `title` and `openapi_url`.

### SEC-2: No UTF-8 Validation for WebSocket Text Frames [LOW]

**File:** `cpp_core/src/ws_frame_parser.cpp` line 659

The C++ parser uses `PyUnicode_DecodeUTF8` with `"surrogateescape"` error handler, accepting invalid UTF-8 rather than failing the connection per RFC 6455 section 5.6.

### SEC-3: No Size Limit on HTTP Buffer [HIGH]

Covered in PERF-1. This is also a security vulnerability (DoS).

---

## 6. Starlette Dependency Audit

### Complete Import Map

**24 files** in `fastapi/` import from Starlette. **~80 individual import statements** across **~15 distinct Starlette modules**.

#### Class Inheritance (Deep Coupling)

| FastAPI Class | Starlette Base | Used At Runtime |
|--------------|----------------|-----------------|
| `FastAPI` | `Starlette` | YES — `__call__`, `build_middleware_stack`, lifespan, routes |
| `APIRoute` | `starlette.routing.Route` | YES — URL matching, `compile_path`, `matches()` |
| `APIRouter` | `starlette.routing.Router` | YES — route dispatch, `add_route`, `routes` |
| `APIWebSocketRoute` | `starlette.routing.WebSocketRoute` | YES — WebSocket URL matching |
| `HTTPException` | `starlette.exceptions.HTTPException` | YES — raised and caught throughout |
| `WebSocketException` | `starlette.exceptions.WebSocketException` | YES — raised during WS handling |
| `BackgroundTasks` | `starlette.background.BackgroundTasks` | YES — post-response task scheduling |
| `UploadFile` | `starlette.datastructures.UploadFile` | YES — file upload handling |

#### Runtime Dependencies (Called Per-Request)

| Starlette Component | Where Used | C++ Replacement? |
|---------------------|------------|------------------|
| `Request(scope, receive, send)` | `routing.py:117` | NO — created per-request |
| `WebSocket(scope, receive, send)` | `routing.py:156` | Partial (`CppWebSocket` in `app.run()` mode) |
| `Request.query_params` | `dependencies/utils.py` | YES — `parse_query_string` in C++ |
| `Request.headers` | `dependencies/utils.py`, `security/*.py` | YES — `parse_scope_headers` in C++ |
| `Request.cookies` | `dependencies/utils.py` | YES — `parse_cookie_header` in C++ |
| `Request.form()` | `dependencies/utils.py` | YES — `parse_multipart_body` in C++ |
| `Request.body()` | `dependencies/utils.py` | YES — handled in C++ fast path |
| `JSONResponse.render()` | `routing.py`, responses | YES — `build_response` in C++ |
| `Response.__call__()` | `routing.py`, throughout | NO — ASGI send protocol |
| `run_in_threadpool` | `routing.py:113`, `dependencies/utils.py` | NO — inherently Python threadpool |
| `is_async_callable` | `routing.py:112` | NO — Python introspection |
| `wrap_app_handling_exceptions` | `routing.py:140,166` | NO — Python exception dispatch |
| `compile_path` | `routing.py` at route registration | YES — C++ trie-based router |
| `ServerErrorMiddleware` | `applications.py:1029` | Partial (500 JSON in C++) |
| `ExceptionMiddleware` | `applications.py:1033` | Partial (HTTPException in C++) |

#### Pure Re-exports (Could Be Changed to C++ Equivalents)

| FastAPI Module | Re-exports From Starlette |
|----------------|--------------------------|
| `fastapi/responses.py` | `Response`, `JSONResponse`, `HTMLResponse`, `PlainTextResponse`, `RedirectResponse`, `FileResponse`, `StreamingResponse` |
| `fastapi/requests.py` | `Request`, `HTTPConnection` |
| `fastapi/websockets.py` | `WebSocket`, `WebSocketDisconnect`, `WebSocketState` |
| `fastapi/staticfiles.py` | `StaticFiles` |
| `fastapi/testclient.py` | `TestClient` |
| `fastapi/templating.py` | `Jinja2Templates` |
| `fastapi/datastructures.py` | `URL`, `Address`, `FormData`, `Headers`, `QueryParams`, `State` |
| `fastapi/concurrency.py` | `run_in_threadpool`, `iterate_in_threadpool`, `run_until_first_complete` |
| `fastapi/__init__.py` | `status` module |
| `fastapi/middleware/*.py` | All 6 middleware classes |

---

## 7. Middleware Architecture & Gaps

### Middleware Stack (Standard ASGI Path)

```
Request ──► ServerErrorMiddleware  (Starlette — catches uncaught exceptions)
        ──► [User Middlewares]     (Starlette — CORS, GZip, custom, etc.)
        ──► ExceptionMiddleware    (Starlette — HTTPException dispatch)
        ──► AsyncExitStackMiddleware (FastAPI — context cleanup)
        ──► Router                 (Starlette — URL matching + dispatch)
```

### Per-Middleware C++ Coverage

| Middleware | Source | C++ in `handle_http`? | C++ in `handle_and_respond`? | Missing C++ Features |
|---|---|---|---|---|
| **CORSMiddleware** | Starlette | **FULL** | **FULL** | `allow_origin_regex`, `allow_private_network`, wildcard header mirroring |
| **GZipMiddleware** | Starlette | **PARTIAL** (+Brotli) | NO | Streaming compression, configurable level, content-type exclusion, `Vary` header |
| **TrustedHostMiddleware** | Starlette | **FULL** | **FULL** | `*.example.com` wildcard subdomain, `www_redirect` |
| **HTTPSRedirectMiddleware** | Starlette | **NO** | **NO** | Entire implementation missing |
| **WSGIMiddleware** | Starlette (deprecated) | NO | NO | N/A (not worth C++) |
| **AsyncExitStackMiddleware** | FastAPI | NO | NO | N/A (Python-only concept) |
| **ServerErrorMiddleware** | Starlette internal | **PARTIAL** (500 JSON) | **PARTIAL** | Debug traceback HTML, custom error handlers |
| **ExceptionMiddleware** | Starlette internal | **PARTIAL** (HTTPException) | **PARTIAL** | Custom exception handlers, status-code handlers |
| **BaseHTTPMiddleware** | Starlette internal | NO | NO | N/A (user code wrapper) |

### The Critical Gap

**When ANY user middleware is present, the C++ fast path is completely bypassed:**

```python
# fastapi/applications.py line 1268-1280
async def __call__(self, scope, receive, send):
    if (
        scope["type"] == "http"
        and self._core_asgi._fast_routes_registered
        and not self.user_middleware        # <-- THIS CONDITION
    ):
        if await self._core_asgi.handle_fast(scope, receive, send):
            return
    await super().__call__(scope, receive, send)  # Full Starlette fallback
```

This means:
- `app.add_middleware(CORSMiddleware, ...)` → **ALL C++ optimization disabled**
- `app.add_middleware(GZipMiddleware, ...)` → **ALL C++ optimization disabled**
- `@app.middleware("http")` → **ALL C++ optimization disabled**

The C++ core has its own CORS, trusted host, and compression implementations, but they are **only used in `app.run()` mode** or when no middleware is configured.

---

## 8. FastAPI Feature Completeness

### User-Facing API: 100% Complete

All 20 top-level exports, all response types, all security utilities, all parameter functions, all middleware re-exports, OpenAPI 3.1.0 generation, dependency injection, Pydantic v2 integration — all present and identical to standard FastAPI 0.128.3.

| Category | Count | Status |
|----------|-------|--------|
| Top-level exports | 20 | All present |
| Response types | 9 (+ UJSONResponse, ORJSONResponse) | All present |
| Security classes | 15 | All present |
| Middleware | 6 re-exports + 1 custom | All present |
| Param functions | 9 (Path, Query, Header, Cookie, Body, Form, File, Depends, Security) | All present |
| OpenAPI models | 30+ classes | All present (3.1.0) |
| Dependency injection | Full `Dependant` system | Complete |
| Exception classes | 11 | All present |
| Data structures | 9 re-exports + `UploadFile` extension | All present |

### Deviation: Hard C++ Dependency (No Fallback)

**File:** `fastapi/_core_bridge.py`

```python
from fastapi._fastapi_core import (  # type: ignore[import-not-found]
    CoreApp, InlineResult, ...
)
# Core extension is REQUIRED -- no Python fallback.
```

Unlike the original Rust architecture (which had `RUST_AVAILABLE=False` graceful fallback), the C++ bridge has **zero fallback**. If `_fastapi_core` is not compiled, the entire package fails to import.

**Affected files that will crash:**
- `fastapi/applications.py` — imports from `_core_bridge`
- `fastapi/routing.py` — imports from `_core_bridge`
- `fastapi/dependencies/utils.py` — imports from `_core_bridge`
- `fastapi/encoders.py` — imports `fast_jsonable_encode`
- `fastapi/exception_handlers.py` — imports `serialize_error_response`

---

## 9. Missing Features in `app.run()` Mode

These features work correctly under uvicorn/ASGI but are **missing or broken when using `app.run()`** (C++ HTTP server):

| Feature | Status | Impact |
|---------|--------|--------|
| **User middleware** (`@app.middleware("http")`, `BaseHTTPMiddleware`) | **Silently ignored** | All custom middleware has no effect |
| **Lifespan context manager** | **Not supported** | Only `on_startup`/`on_shutdown` work |
| **StreamingResponse** | **Not supported** | Response built as single blob |
| **FileResponse** | **Not supported** | No file serving |
| **BackgroundTasks** | **Not supported** | No post-response task mechanism |
| **`Request` object** | **Not created** | `Request` dependency injection fails |
| **Exception handlers** (`app.exception_handler()`) | **Not called** | Generic 500 for all errors |
| **Debug mode tracebacks** | **Not supported** | No HTML error pages |
| **HTTP/2** | Not supported | HTTP/1.1 only |
| **Chunked transfer encoding** | Not supported | — |
| **Server-Sent Events** | Not supported | No streaming |
| CORS | Partially handled | Config extracted from middleware list |
| GZip compression | C++ native (+ Brotli) | Works, better than Starlette |
| Status reason phrase | Always "OK" | `HTTP/1.1 404 OK` instead of `Not Found` |
| Exception logging | **None** | All errors silently swallowed |
| Graceful connection draining | **None** | In-flight requests dropped on shutdown |
| Keep-alive timeout | Hardcoded 15s | Not configurable |

---

## 10. Thread Safety Concerns

### TS-1: Static Cache Lazy Init Without Locks [MEDIUM]

**File:** `cpp_core/src/app.cpp` lines 27-33, `cpp_core/src/json_writer.cpp`

Static `PyObject*` caches are lazily initialized without synchronization. Safe under GIL, but **will break under free-threaded Python 3.13+** (PEP 703).

**Fix:** Use `std::call_once` or `std::atomic<PyObject*>`.

### TS-2: WebSocket Task Not Tracked/Cancelled [MEDIUM]

**File:** `fastapi/_cpp_server.py` line 906

`create_task()` stores no reference. On `connection_lost`, the task is not cancelled. Endpoints blocked on non-`receive_*` operations will hang.

**Fix:** Store task reference, cancel on `connection_lost`.

### TS-3: `MatchParams::add` Silently Drops >4 Params [LOW]

**File:** `cpp_core/include/router.hpp` line 31

```cpp
void add(std::string_view n, std::string_view v) {
    if (param_count < MAX_INLINE) params[param_count++] = {n, v};
}
```

Routes with > 4 path parameters silently lose excess params. No warning or error.

### TS-4: `_ws_sig_cache` / `_endpoint_context_cache` Keyed by `id()` [LOW]

**Files:** `fastapi/_cpp_server.py` line 163, `fastapi/routing.py` line 257

`id()` values can be reused after garbage collection. Stale cache entries could be served for different functions. Low risk since endpoints are module-level singletons.

### TS-5: `_WsServerMetrics` Not Thread-Safe [LOW]

**File:** `fastapi/_cpp_server.py` lines 264-273

Plain integer attributes modified from the event loop. Safe under GIL, but will need atomic operations for free-threaded Python.

---

## 11. Dead / Unused Code

| Code | File | Notes |
|------|------|-------|
| `_WsRateLimiter` class | `fastapi/_cpp_server.py:704-722` | Defined, never instantiated |
| `py_ws_parse_frames` | `cpp_core/src/ws_frame_parser.cpp:470` | Legacy, replaced by `*_direct` handlers |
| `py_ws_parse_frames_text` | `cpp_core/src/ws_frame_parser.cpp:633` | Legacy, replaced by `*_direct` handlers |
| `py_ws_parse_frames_json` | `cpp_core/src/websocket_handler.cpp:131` | Replaced by `py_ws_handle_json_direct` |
| `cleanup_cached_refs()` | `cpp_core/src/app.cpp` | Defined but never called |
| `generate_operation_id_for_path()` | `fastapi/utils.py:92-104` | Deprecated, `pragma: nocover` |
| `generate_operation_id()` | `fastapi/openapi/utils.py:217-229` | Deprecated, `pragma: nocover` |
| `version` parameter | `fastapi/utils.py:73` | Accepted but never used |
| `self.in_ = self.in_` | `fastapi/params.py:187` | No-op self-assignment |
| `_Attrs` dict + `asdict()` | `fastapi/_compat/v2.py:51-87` | Has `TODO: remove` comment |
| `example_server.py` | Root directory | Empty file (0 bytes) |
| Stale `.pyc` files | `__pycache__/` | `_rust_bridge.pyc`, `_rust_app.pyc` from old Rust arch |

---

## 12. C++ Code Strengths

The C++ core is exceptionally well-engineered:

| Feature | Implementation | Benefit |
|---------|---------------|---------|
| **Pre-interned ASGI strings** | `asgi_constants.cpp` — all keys + status codes pre-cached | Zero per-request string creation or hashing |
| **Two-phase router** | O(1) hash map for static routes + radix trie with first-byte dispatch for parametric | Sub-microsecond route matching |
| **SIMD WebSocket unmasking** | AVX2 (32-byte), SSE2 (16-byte), NEON (16-byte) + scalar fallback | 4-8x faster than byte-by-byte |
| **Buffer pool** | Thread-local storage, max 32 buffers, `clear()` preserves capacity | Near-zero per-request heap allocation |
| **Streaming JSON writer** | `itoa` + `ryu`, batch-scan string escaping, no `serde_json::Value` | Minimal allocations, no intermediate DOM |
| **`PyRef` RAII wrapper** | Move-only, `Py_XDECREF` in destructor, `borrow()` + `release()` | Consistent refcount management throughout |
| **`shared_mutex` + `routes_frozen`** | Shared lock for reads, atomic flag for lock-free after startup | Lock-free hot path |
| **`atomic<shared_ptr<CorsConfig>>`** | C++20 atomic shared_ptr for config swap | Lock-free CORS config updates |
| **PyCapsule lifecycle** | Destructor for C++ objects tied to Python GC | No manual cleanup needed |
| **GIL release** | `Py_BEGIN_ALLOW_THREADS` in all CPU-heavy paths, skip for < 256 bytes | True parallelism for compression/parsing |
| **Inline storage for params** | `MatchParams` inline array[4] | Zero heap allocation for 99%+ of routes |

### Benchmark Results

| Test | C++ msg/sec | Python msg/sec | Speedup |
|------|-------------|----------------|---------|
| WS Echo (small 17B) | 141,933 | 15,312 | **9.3x** |
| WS Echo (medium 256B) | 129,487 | 14,864 | **8.7x** |
| WS Echo (large 4KB) | 70,268 | 10,485 | **6.7x** |
| WS JSON Echo (small) | 60,954 | 12,258 | **5.0x** |
| WS JSON Echo (medium) | 55,950 | 12,365 | **4.5x** |
| WS Throughput (100) | 3,707 | 511 | **7.3x** |
| WS Echo High Conc (500) | 103,581 | 12,674 | **8.2x** |

---

## 13. C++ Replacement Roadmap for Starlette

### What Should Be Replaced (High ROI)

#### Phase 1: Middleware Integration (Highest Impact)

**Goal:** Allow C++ fast path to work WITH user middleware, not be disabled by it.

| Component | Current State | C++ Work Needed | Effort |
|-----------|--------------|-----------------|--------|
| CORS middleware | C++ exists, but fast path disabled when added | Integrate C++ CORS into ASGI fast path; add `allow_origin_regex`, wildcard header mirroring | Medium |
| GZip middleware | C++ exists for `app.run()` only | Add streaming compression, `Vary` header, content-type exclusion; wire into ASGI path | Medium |
| Trusted host | C++ exists | Add `*.example.com` wildcard, `www_redirect` | Low |
| HTTPS redirect | Missing | Add ~30 lines in C++ to check scheme + build redirect | Low |
| ExceptionMiddleware | Partial (HTTPException only) | Add custom handler dispatch table lookup in C++ | Medium |
| ServerErrorMiddleware | Partial (500 JSON only) | Add custom error handler support; keep Python for debug HTML | Low |

**The key architectural change needed:** Instead of `not self.user_middleware` disabling C++ entirely, the C++ fast path should handle known middleware types natively and only fall back for truly custom middleware.

#### Phase 2: Request/Response Objects

| Component | Current State | C++ Work Needed | Effort |
|-----------|--------------|-----------------|--------|
| `Request` object | Created by Starlette per-request | Create lightweight C++ request proxy that reads from parsed scope | High |
| `Response`/`JSONResponse` | Starlette classes | Already have `build_response` in C++; need ASGI `__call__` compat | Medium |
| `HTMLResponse` | Starlette | Trivial wrapper around text response with content-type header | Low |
| `StreamingResponse` | Starlette, not in C++ server | Implement chunked response writing in C++ protocol | High |
| `FileResponse` | Starlette | Implement `sendfile()` syscall path | Medium |

#### Phase 3: Routing Core

| Component | Current State | C++ Work Needed | Effort |
|-----------|--------------|-----------------|--------|
| `compile_path` | Starlette regex compilation | Already replaced by C++ trie; need to fully bypass Starlette regex | Medium |
| `Route.matches()` | Starlette regex matching | Already replaced in C++ fast path; need fallback elimination | Medium |
| `Router.route()` dispatch | Starlette | Already done in C++ for fast path | Done |
| `Mount` support | Starlette only | Add mount/sub-app support to C++ router | High |

#### Phase 4: Data Structures

| Component | Current State | C++ Work Needed | Effort |
|-----------|--------------|-----------------|--------|
| `QueryParams` | Starlette `ImmutableMultiDict` | Already have `parse_query_string` in C++; need Python-compatible wrapper | Medium |
| `Headers` | Starlette `Headers` class | Already have `parse_scope_headers` in C++; need Python wrapper | Medium |
| `FormData` | Starlette | Already have `parse_multipart_body` in C++ | Medium |
| `State` | Starlette `State` | Simple Python dict wrapper, not worth C++ | Skip |
| `URL`, `Address` | Starlette | Low-frequency, not worth C++ | Skip |

### What Should NOT Be Replaced (Low ROI / Impossible)

| Component | Reason |
|-----------|--------|
| `BaseHTTPMiddleware` | Wraps arbitrary user Python code |
| `run_in_threadpool` | Python threadpool for sync endpoints |
| `is_async_callable` | Python introspection |
| `wrap_app_handling_exceptions` | Python exception dispatch |
| `TestClient` | Testing only, not performance-critical |
| `StaticFiles` | File I/O already at OS speed |
| `Jinja2Templates` | Template rendering is Jinja2, not Starlette |
| `WSGIMiddleware` | Deprecated, wraps Python WSGI apps |
| `AsyncExitStackMiddleware` | Python async context managers |
| `BackgroundTasks` | Runs Python callables post-response |
| Pydantic integration (`_compat/`) | Deeply coupled to Pydantic Python internals |
| OpenAPI generation | Runs once, depends on Python type introspection |

---

## 14. Priority Action Items

### Tier 1: Critical Bugs (Fix Immediately)

| # | Issue | File | Est. Effort |
|---|-------|------|-------------|
| 1 | Double-free in exception handling (BUG-1) | `app.cpp:1198` | 30 min |
| 2 | Reference leak in `get_response_filters` (BUG-2) | `app.cpp:711` | 10 min |
| 3 | Cork + `send_json` double-framing (BUG-3) | `_cpp_server.py:474` | 1 hr |
| 4 | Missing `_write_frame` method (BUG-4) | `_ws_groups.py:83` | 30 min |

### Tier 2: Security & Correctness (Fix Soon)

| # | Issue | File | Est. Effort |
|---|-------|------|-------------|
| 5 | HTTP buffer size limit (PERF-1 / SEC-3) | `_cpp_server.py:811` | 1 hr |
| 6 | Byte-count WebSocket backpressure (PERF-2) | `_cpp_server.py:54` | 2 hr |
| 7 | WebSocket scope missing fields (BUG-5) | `_cpp_server.py:325` | 2 hr |
| 8 | WebSocket `accept()` subprotocol (BUG-6) | `_cpp_server.py:335` | 1 hr |
| 9 | OpenAPI `status_code` uninitialized (BUG-7) | `openapi/utils.py:344` | 30 min |
| 10 | XSS in docs HTML (SEC-1) | `openapi/docs.py:138` | 30 min |

### Tier 3: Memory & Robustness (Fix Next)

| # | Issue | File | Est. Effort |
|---|-------|------|-------------|
| 11 | `WsDeflateContext` destructor (LEAK-1) | `ws_frame_parser.cpp:958` | 30 min |
| 12 | Global registry cleanup (LEAK-2) | `param_extractor.cpp`, `dep_engine.cpp` | 1 hr |
| 13 | Static ref cleanup (LEAK-3) | `app.cpp:27` | 30 min |
| 14 | Add logging to `except Exception: pass` | Multiple files | 2 hr |
| 15 | Track/cancel WebSocket tasks (TS-2) | `_cpp_server.py:906` | 1 hr |

### Tier 4: Architecture (Next Sprint)

| # | Issue | Est. Effort |
|---|-------|-------------|
| 16 | Allow C++ fast path WITH known middleware types | 1-2 weeks |
| 17 | Add lifespan context manager support to `app.run()` | 2 days |
| 18 | Add StreamingResponse support to C++ server | 3-5 days |
| 19 | Add custom exception handler support to C++ path | 2-3 days |
| 20 | Wire security classes through C++ extraction | 1-2 days |

### Tier 5: Cleanup (When Convenient)

| # | Issue | Est. Effort |
|---|-------|-------------|
| 21 | Remove dead code (11 items listed in Section 11) | 2 hr |
| 22 | Deduplicate `handle_and_respond` / `handle_request_inline` (~400 lines) | 4 hr |
| 23 | Future-proof static caches for free-threaded Python | 2 hr |
| 24 | Fix status reason phrase ("OK" for all codes) | 30 min |
| 25 | Add Python fallback to `_core_bridge.py` | 4 hr |
| 26 | Delete stale Rust `.pyc` files | 5 min |
| 27 | Delete empty `example_server.py` | 5 min |

---

## Appendix A: Complete Starlette Import Map

```
fastapi/applications.py        → Starlette, State, HTTPException, Middleware,
                                  BaseHTTPMiddleware, ServerErrorMiddleware,
                                  ExceptionMiddleware, Request, HTMLResponse,
                                  JSONResponse, Response, BaseRoute, ASGIApp,
                                  ExceptionHandler, Lifespan, Receive, Scope, Send

fastapi/routing.py             → routing (Route, Router, WebSocketRoute),
                                  wrap_app_handling_exceptions, is_async_callable,
                                  run_in_threadpool, HTTPException, Request,
                                  JSONResponse, Response, BaseRoute, Match,
                                  compile_path, get_name, Mount, AppType, ASGIApp,
                                  Lifespan, Receive, Scope, Send, WebSocket,
                                  FormData, Headers, UploadFile

fastapi/_core_app.py           → HTTPException, Response, Receive, Scope, Send

fastapi/_cpp_server.py         → WebSocketDisconnect, WebSocketState

fastapi/dependencies/utils.py  → StarletteBackgroundTasks, run_in_threadpool,
                                  FormData, Headers, ImmutableMultiDict,
                                  QueryParams, UploadFile, HTTPConnection,
                                  Request, Response, WebSocket

fastapi/exception_handlers.py  → HTTPException, Request, JSONResponse, Response,
                                  WS_1008_POLICY_VIOLATION

fastapi/exceptions.py          → StarletteHTTPException, StarletteWebSocketException

fastapi/background.py          → StarletteBackgroundTasks

fastapi/concurrency.py         → iterate_in_threadpool, run_in_threadpool,
                                  run_until_first_complete

fastapi/datastructures.py      → URL, Address, FormData, Headers, QueryParams,
                                  State, StarletteUploadFile

fastapi/responses.py           → FileResponse, HTMLResponse, JSONResponse,
                                  PlainTextResponse, RedirectResponse, Response,
                                  StreamingResponse

fastapi/requests.py            → HTTPConnection, Request

fastapi/websockets.py          → WebSocket, WebSocketDisconnect, WebSocketState

fastapi/__init__.py            → status

fastapi/staticfiles.py         → StaticFiles
fastapi/testclient.py          → TestClient
fastapi/templating.py          → Jinja2Templates

fastapi/middleware/__init__.py  → Middleware
fastapi/middleware/cors.py      → CORSMiddleware
fastapi/middleware/gzip.py      → GZipMiddleware
fastapi/middleware/httpsredirect.py → HTTPSRedirectMiddleware
fastapi/middleware/trustedhost.py   → TrustedHostMiddleware
fastapi/middleware/wsgi.py      → WSGIMiddleware
fastapi/middleware/asyncexitstack.py → ASGIApp, Receive, Scope, Send

fastapi/security/api_key.py    → HTTPException, Request, HTTP_401_UNAUTHORIZED
fastapi/security/http.py       → Request, HTTP_401_UNAUTHORIZED
fastapi/security/oauth2.py     → Request, HTTP_401_UNAUTHORIZED
fastapi/security/open_id_connect_url.py → HTTPException, Request, HTTP_401_UNAUTHORIZED

fastapi/openapi/docs.py        → HTMLResponse
fastapi/openapi/utils.py       → JSONResponse, BaseRoute

fastapi/_compat/shared.py      → UploadFile
```

---

*Report generated by automated code review. All findings verified against source code.*
