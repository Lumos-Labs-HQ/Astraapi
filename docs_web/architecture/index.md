# Architecture Overview

AstraAPI is built on a **hybrid architecture**: you write Python, but HTTP parsing, routing, serialization, and response writing happen in a **C++ core** that integrates directly with Python's C API and asyncio event loop. Unlike other frameworks, AstraAPI includes a **built-in multi-worker server** — no gunicorn or uvicorn required.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Your Python Code                          │
│  @app.get("/")                                                   │
│  def hello():                                                    │
│      return {"msg": "hi"}                                        │
└──────┬───────────────────────────────────────────────────────────┘
       │ Python C API (PyObject_Call)
┌──────▼───────────────────────────────────────────────────────────┐
│                      C++ HTTP Core                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐     │
│  │  llhttp  │──│  Router  │──│ Endpoint │──│   Custom     │     │
│  │  Parser  │  │ (C++)    │  │  Caller  │  │  Serializer  │     │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘     │
│       │                                    │                     │
│       │ Zero-copy parsing                  │ SIMD + ryu floats   │
│       ▼                                    ▼                     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │         write_response_direct() / writev()                │   │
│  │    (bypasses Python transport for small responses)        │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────┬───────────────────────────────────────────────────────────┘
       │ socket write (send() / writev())
┌──────▼───────────────────────────────────────────────────────────┐
│                     Kernel / Network                             │
└──────────────────────────────────────────────────────────────────┘
```

## Design Goals

1. **100% API Compatibility** — AstraAPI passes the full FastAPI test suite (3730+ tests). No code changes required to switch.
2. **Zero-Copy Where Possible** — llhttp parses headers in-place. Responses are written directly from C++ buffers.
3. **No External Server Required** — Built-in multi-worker process manager with `SO_REUSEPORT`, CPU affinity, and automatic restart.
4. **Amortize Python Overhead** — The expensive work (parsing, routing, JSON serialization) happens in C++. Python only runs your endpoint code.
5. **Respect the GIL** — C++ operations hold the GIL briefly and yield back to Python's event loop for true async concurrency.

## Key Components

| Component | Language | Responsibility |
|-----------|----------|--------------|
| **HTTP Parser** | C++ (llhttp) | Parse HTTP/1.1 requests with zero-copy callbacks |
| **Router** | C++ | O(1) static hash map + radix trie for parametric routes |
| **Endpoint Caller** | C++ | Call Python endpoint via `PyObject_Call`, drive async coroutines with `PyIter_Send` |
| **JSON Parser** | C++ (yyjson) | ~3GB/s JSON parsing with 2-phase GIL-release API |
| **JSON Serializer** | C++ (custom) | Streaming writer with SIMD string escape, ryu float formatting |
| **Transport Writer** | C++ | Direct `send()` bypass for small responses; `writev()` for WebSockets |
| **Compression** | C++ (libdeflate) | 2-3× faster gzip than zlib; optional brotli |
| **Python Protocol** | Python | asyncio Protocol bridge — connection lifecycle, batch keep-alive, WebSocket upgrade |
| **App Class** | Python | Route registration, middleware stack, dependency injection, OpenAPI generation |
| **Worker Manager** | Python | Multi-process fork/spawn with `SO_REUSEPORT` or `SCM_RIGHTS` fd dispatch |

## The Request Lifecycle

```
1. Kernel receives bytes on socket
        │
        ▼
2. asyncio selector / epoll
        │
        ▼
3. CppHttpProtocol.data_received(data)
        │
        ▼
4. C++ core: handle_http_append_and_dispatch
   ├─ Fast-path GET/HEAD scan (before llhttp)
   ├─ llhttp parse (zero-copy)
   ├─ Route match: static hash → radix trie
   ├─ Extract path/query/header/cookie params
   ├─ Body parse: yyjson (GIL released) / multipart / form
   ├─ Call endpoint (PyObject_Call)
   └─ Serialize response: custom writer → buffer pool
        │
        ▼
5. C++: write_response_direct() → POSIX send()
   OR: write_to_transport() → cached bound method
        │
        ▼
6. Kernel sends bytes to client
```

## Why It's Fast

| Bottleneck in Pure Python | AstraAPI Solution | Speedup |
|---------------------------|-------------------|---------|
| `json.dumps()` per request | Custom C++ streaming serializer + SIMD escape + ryu | 5-10× |
| `h11` / `httptools` parser | llhttp (Node.js parser) + fast-path GET scanner | 2-3× |
| Python dict for route table | C++ unordered_map (static) + radix trie (dynamic) | 3-5× |
| `transport.write()` method lookup | Direct `send()` syscall for small responses | ~700ns saved/call |
| zlib gzip compression | libdeflate (2-3× faster) | 2-3× |
| GC pauses under load | `gc.disable()` + `gc.freeze()` + protocol pooling | Eliminates jitter |
| Per-connection keep-alive timers | Batch sweep every 10 seconds | ~1000× fewer callbacks |
| Coroutine task creation overhead | `PYGEN_RETURN` fast path (no Python task) | Skips create_task |
| External server overhead | Built-in multi-worker (no gunicorn/uvicorn) | Lower latency |

## Built-in Server Architecture

```
User calls app.run(workers=4)
        │
        ├─► Linux: fork() 4 workers
        │     ├─ SO_REUSEPORT: each worker binds own socket (kernel LB)
        │     └─ Fallback: master accept() + SCM_RIGHTS fd dispatch
        │
        ├─► Windows: spawn 4 subprocesses
        │     └─ SO_REUSEADDR: each worker binds independently
        │
        └─► Single process: loop.create_server()

Each worker:
  ├─ Own GIL, event loop (uvloop preferred), memory space
  ├─ Pre-warmed protocol pool (1024 objects)
  ├─ C++ buffer pool pre-allocated
  ├─ CPU affinity pinned to dedicated core
  └─ Parent monitors via waitpid(); restarts on crash
```

## Compatibility Layer

AstraAPI doesn't depend on Starlette. It **reimplements** the hot path while maintaining 100% API compatibility:

- **No Starlette imports** at runtime — `AppBase`, `Router`, `Request`, `Response`, `WebSocket`, `StaticFiles`, `TestClient`, and all middleware are native
- **Pydantic v2** exclusively — validation and OpenAPI schema generation
- **Lazy imports** — `__init__.py` defers heavy imports until first access, cutting import time by ~70%

## Deep Dives

- [**C++ Core**](./cpp-core) — llhttp, yyjson, SIMD serializer, radix trie router, libdeflate
- [**Python Asyncio Bridge**](./python-bridge) — How C++ and asyncio cooperate, batch keep-alive, protocol pools
- [**HTTP Pipeline**](./http-pipeline) — Request lifecycle in detail
- [**Memory Model**](./memory-model) — GIL safety, reference counting, buffer pools, object lifetimes
- [**Zero-Copy & Caching**](./zero-copy) — Response cache, direct FD writes, buffer management
