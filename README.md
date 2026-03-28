<h1 align="center">
  <br>
  ⚡ AstraAPI
  <br>
</h1>

<p align="center">
  <b>High-Performance · Easy to Learn · Fast to Code · Ready for Production</b>
</p>

<p align="center">
  <a href="https://pypi.org/project/astraapi"><img alt="PyPI" src="https://img.shields.io/pypi/v/astraapi?color=blue&label=PyPI&logo=pypi"></a>
  <a href="https://github.com/Lumos-Labs-HQ/Astraapi/blob/main/LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-green"></a>
  <img alt="Status" src="https://img.shields.io/badge/status-beta-yellow">
  <img alt="Python" src="https://img.shields.io/badge/python-3.9%2B-blue">
  <img alt="C++20" src="https://img.shields.io/badge/core-C%2B%2B20-orange">
  <img alt="Pydantic v2" src="https://img.shields.io/badge/pydantic-v2-teal">
</p>

---

**AstraAPI** is a modern, production-grade Python web framework with a **C++20 hot-path core**. It is **inspired by and fully API-compatible with [FastAPI](https://fastapi.tiangolo.com/)** — you get the same beautiful decorator-based routing, automatic OpenAPI docs, Pydantic v2 validation, Depends injection, and WebSocket support, while the inner engine is a compiled C++ extension that handles HTTP parsing, route matching, parameter extraction, JSON serialization, CORS, compression, and response building — with zero Python overhead on the critical path.

> **"AstraAPI = FastAPI's developer experience + uWebSockets' throughput + Node.js cluster's multi-worker model"**

---

## ✨ Key Features

| Feature | Description |
|---------|-------------|
| 🚀 **C++20 HTTP Core** | HTTP parsing, route matching, param extraction, JSON encode/decode — all in compiled C++ (`_astraapi_core.so`) |
| 🎯 **FastAPI-Compatible API** | Same decorators (`@app.get`, `@app.post`, …), `Depends`, `Body`, `Query`, `Path`, `Header`, `Cookie`, `Form`, `File`, `Security` |
| 🔄 **Zero-Lock Multi-Worker** | Each worker is a fully independent OS process with its own GIL, event loop, and memory space. Zero shared state = zero lock contention |
| 🌀 **uvloop / winloop Event Loop** | Uses uvloop (Linux/macOS) or winloop (Windows) instead of CPython's default asyncio loop — up to 2–4× more I/O throughput |
| 📄 **Auto OpenAPI + Swagger/ReDoc** | Built-in `/docs` (Swagger UI) and `/redoc` (ReDoc) — served directly from C++ pre-built byte buffers |
| ✅ **Pydantic v2 Validation** | Full request body validation, response model serialization, and `model_dump_json` — all via Pydantic v2 |
| 🛡️ **Built-in Middleware** | CORS, TrustedHost, GZip/Brotli compression, Rate Limiting — implemented in C++ with zero Python allocation per request |
| 🔌 **WebSocket Support** | RFC 6455 compliant WebSocket with C++ ring-buffer frame parsing, echo auto-detection, batch frame building, and per-connection backpressure |
| ⚙️ **Lazy Imports** | Heavy imports (Pydantic, OpenAPI models) are deferred until first access. Cold startup reduced from ~4.7 s → ~1–1.5 s |
| 🔁 **Hot Reload** | File-watching reloader (`watchfiles`) restarts workers on code change |
| 🧪 **Test Client** | Drop-in `TestClient` based on `httpx` for sync/async endpoint testing |
| 📁 **Static Files + Jinja2 Templates** | Plug-and-play static file serving and HTML templating |
| 🔒 **Security** | OAuth2, HTTP Basic/Bearer, API Key — mirrors FastAPI's security utilities |
| 🪝 **Lifespan Events** | `on_startup` / `on_shutdown` callbacks and async context-manager `lifespan` |
| 🖥️ **Cross-Platform** | Linux (SO_REUSEPORT + uvloop), macOS (fork + uvloop), Windows (subprocess + winloop + socket.share) |

---

## 🌟 Inspired by FastAPI

AstraAPI was **directly inspired by [FastAPI](https://fastapi.tiangolo.com/)** created by [Sebastián Ramírez](https://github.com/tiangolo). Every public-facing API — decorators, dependency injection, parameter declarations, OpenAPI generation, security utilities, response models — is intentionally compatible with FastAPI so that existing FastAPI applications can be migrated with minimal changes.

```python
# This is valid AstraAPI code — and also valid FastAPI code
from astraapi import AstraAPI, Depends
from pydantic import BaseModel

app = AstraAPI(title="My API")

class Item(BaseModel):
    name: str
    price: float

@app.post("/items/", response_model=Item)
async def create_item(item: Item):
    return item
```

What AstraAPI adds on top of the FastAPI mental model:
- A **compiled C++20 extension** replaces the entire ASGI hot path
- A **built-in multi-worker supervisor** replaces Gunicorn/Uvicorn
- A **uvloop/winloop event loop** replaces the default asyncio loop
- **Zero-allocation per-request interning** for HTTP method strings, header names, status lines

---

## 📦 Installation

```bash
pip install astraapi
```

> **Note:** The package ships a pre-built `_astraapi_core.so` (Linux) / `.pyd` (Windows). To build from source:
> ```bash
> cd cpp_core && mkdir build && cd build
> cmake .. -DCMAKE_BUILD_TYPE=Release
> make -j$(nproc)
> cp _astraapi_core*.so ../../astraapi/_astraapi_core.so
> ```

---

## 🚀 Quick Start

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
async def root():
    return {"message": "Hello from AstraAPI ⚡"}

# Run with built-in multi-worker server
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000, workers=4)
```

---

## 🔄 Complete Request Flow

The following diagram shows the full lifecycle of an HTTP request inside AstraAPI:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          CLIENT  (Browser / HTTP Client)                    │
└────────────────────────────────────┬────────────────────────────────────────┘
                                     │  TCP Connection
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   MASTER PROCESS  (Supervisor / Accept Thread)              │
│                                                                             │
│  ① listen_sock.accept()  — single blocking accept, no thundering herd      │
│  ② Round-robin dispatch  — SCM_RIGHTS (Linux) / socket.share() (Windows)  │
│  ③ Worker restart        — crashed workers are automatically re-spawned    │
└────────────┬──────────────────────────┬────────────────────────────────────┘
             │ fd / shared socket        │ fd / shared socket
      ┌──────▼──────┐            ┌───────▼──────┐
      │  Worker 0   │            │  Worker 1    │   ... (N workers)
      │  (Process)  │            │  (Process)   │
      │  own GIL    │            │  own GIL     │
      │  own loop   │            │  own loop    │
      └──────┬──────┘            └──────────────┘
             │
             ▼  CppHttpProtocol.data_received()
┌─────────────────────────────────────────────────────────────────────────────┐
│                   C++ HTTP LAYER  (_astraapi_core.so)                       │
│                                                                             │
│  ④ HTTP Parse      — llhttp (Node.js parser, zero-copy view)               │
│  ⑤ Route Match     — Radix trie, O(log n), pre-compiled at startup         │
│  ⑥ Param Extract   — Path params, query string, headers, cookies in C++    │
│  ⑦ JSON Parse      — yyjson (SIMD-accelerated, strict mode)                │
│  ⑧ CORS Check      — case-insensitive origin matching, zero allocation     │
│  ⑨ DI Resolution   — Resolve Depends graph, inject typed params in C++     │
│  ⑩ Compression     — libdeflate (gzip) / Brotli, chosen by Accept-Encoding │
│  ⑪ Rate Limiting   — Sharded mutex counters (16 shards), per-IP           │
└──────────────┬──────────────────────────┬──────────────────────────────────┘
               │                          │
   ┌───────────▼─────────┐    ┌───────────▼─────────────┐
   │  SYNC ENDPOINT      │    │  ASYNC / PYDANTIC        │
   │  (no Python touch)  │    │  ENDPOINT                │
   │                     │    │                          │
   │  C++ builds full    │    │  C++ returns InlineResult│
   │  HTTP response and  │    │  Python: await endpoint  │
   │  calls              │    │  Pydantic validates resp │
   │  transport.write()  │    │  C++ serializes JSON     │
   └─────────────────────┘    └──────────────────────────┘
               │                          │
               └──────────┬───────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      RESPONSE PATH                                          │
│                                                                             │
│  C++ build_response_from_parts() → pre-cached status lines + headers →     │
│  transport.write(bytes)  — single syscall, keep-alive maintained           │
└─────────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
                      CLIENT  ✅
```

### Request Flow — Step by Step

1. **TCP Accept** — The master supervisor calls `accept()` once and dispatches the file descriptor to a worker via Unix socket (SCM_RIGHTS on Linux) or `socket.share()` (Windows). Workers **never compete** for connections.
2. **data_received** — Python's asyncio protocol (`CppHttpProtocol`) receives raw bytes and calls into `_astraapi_core` via a single C-extension call.
3. **HTTP Parse** — C++ uses the `llhttp` parser (same as Node.js) to parse headers and body. Strings are interned (`PyUnicode_InternFromString`) once and reused forever.
4. **Route Match** — A compiled radix trie matches the method + path in O(log n). Routes are frozen before workers fork — shared as read-only COW pages.
5. **Parameter Extraction** — Path params, query strings, headers, cookies extracted in C++ with zero Python dict construction for sync routes.
6. **Endpoint Dispatch** — Sync endpoints return directly; async endpoints yield an `InlineResult` capsule back to Python which `await`s the coroutine.
7. **Pydantic Validation** — Response models call `model_dump_json()` — result is passed back to C++ for final serialization.
8. **Response Write** — C++ assembles the full HTTP response from pre-cached byte fragments and calls `transport.write()` — a single syscall.

---

## 🏗️ Worker Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                        ASTRAAPI  PROCESS MODEL                       │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │              MASTER  (Supervisor Process)                    │   │
│   │                                                              │   │
│   │  • listen_sock.accept() ──► round-robin dispatch           │   │
│   │  • Monitors child PIDs via os.waitpid()                    │   │
│   │  • Auto-restarts crashed workers                           │   │
│   │  • Tunes sysctl (somaxconn, tcp_max_syn_backlog)           │   │
│   │  • Raises fd limit (RLIMIT_NOFILE → 65535)                 │   │
│   └──────────┬──────────────┬────────────────┬─────────────────┘   │
│              │              │                │                       │
│    SCM_RIGHTS│   socket.    │  SO_REUSEPORT  │                       │
│    (Linux)   │   share()    │  (kernel does  │  + ─ ─  N workers    │
│              │  (Windows)   │   balancing)   │                       │
│    ┌─────────▼──┐  ┌────────▼──┐  ┌──────────▼──┐                  │
│    │  Worker 0  │  │  Worker 1  │  │  Worker N   │                  │
│    │            │  │            │  │             │                  │
│    │ own GIL    │  │  own GIL   │  │  own GIL    │                  │
│    │ own memory │  │ own memory │  │ own memory  │                  │
│    │ own loop   │  │ own loop   │  │ own loop    │                  │
│    │ uvloop/    │  │ uvloop/    │  │ uvloop/     │                  │
│    │ winloop    │  │ winloop    │  │ winloop     │                  │
│    │            │  │            │  │             │                  │
│    │ CPU pinned │  │ CPU pinned │  │ CPU pinned  │ ← sched_setaff. │
│    │ (core 0)   │  │ (core 1)   │  │ (core N%k)  │                  │
│    └────────────┘  └────────────┘  └─────────────┘                  │
│                                                                      │
│  • Route table frozen before fork → shared as read-only COW pages   │
│  • Zero IPC after startup (SO_REUSEPORT mode)                       │
└──────────────────────────────────────────────────────────────────────┘
```

### Linux — Three Modes (Best → Fallback)

| Mode | How it works | Latency |
|------|-------------|---------|
| **SO_REUSEPORT** | Each worker binds its own socket; kernel dispatches SYN packets directly — no master accept thread, no IPC | Lowest |
| **Master-Accept + SCM_RIGHTS** | Master calls `accept()`, sends fd to worker via `sendmsg + SCM_RIGHTS` over Unix socketpair | Low |
| **Shared socket** | Single listen socket shared via `socket.share()` | Moderate |

### Windows

Master uses `socket.share(pid)` + `socket.fromshare()` over AF_INET socketpairs. Same round-robin guarantee, no thundering herd.

---

## ⚡ Why AstraAPI is Faster Than Uvicorn + Gunicorn

The table below compares what happens on every single request:

| Component | Uvicorn + Gunicorn | AstraAPI |
|-----------|-------------------|----------|
| **HTTP Parsing** | Python ASGI scope dict allocation (7+ keys) | C++ llhttp, zero Python dict |
| **Route Matching** | Python regex, dictionary lookup per route | C++ radix trie, O(log n), pre-compiled |
| **Param Extraction** | Python `re.match()` + dict construction | C++ `param_extractor`, direct struct write |
| **JSON Decode** | Python `json.loads()` / `orjson.loads()` | C++ yyjson SIMD parser |
| **JSON Encode** | Python `json.dumps()` / `orjson.dumps()` | C++ yyjson writer, single allocation |
| **CORS** | Python middleware, dict lookups, `.encode()` calls | C++ case-insensitive origin match, no alloc |
| **GZip** | Python `gzip` module (GIL-bound) | C++ libdeflate (2–3× faster than zlib) |
| **Brotli** | Requires separate `brotli` middleware | C++ Brotli native, auto-negotiated |
| **Response Build** | Python `bytearray` + string formatting | C++ pre-cached status lines + single memcpy |
| **Worker Model** | Gunicorn master-accept → WSGI/ASGI overhead | Zero-lock independent processes, no arbiter GIL |
| **Event Loop** | Default asyncio (libuv-less) | uvloop / winloop (libuv-based, 2–4× faster I/O) |
| **Startup Time** | FastAPI ~4–5 s cold import | AstraAPI ~1–1.5 s (lazy imports) |
| **String Interning** | `.encode()` per request for method/headers | Pre-interned `PyUnicode_InternFromString` at startup |
| **Memory per request** | Multiple dict + list allocations in Python | Reused C++ buffer pool (`buffer_pool.cpp`) |

### Key Architectural Advantages

**1. C++ on the hot path, Python only where needed**
```
Sync endpoint:   TCP bytes → C++ parse → C++ route → C++ params → C++ response  (zero Python)
Async endpoint:  TCP bytes → C++ parse → C++ route → C++ params → Python await → C++ serialize
```

**2. Zero thundering herd**

Gunicorn's default pre-fork model has all workers call `accept()` simultaneously, causing a kernel stampede on connection arrival. AstraAPI's master process accepts once and dispatches — workers consume from a queue with guaranteed even distribution (Node.js cluster `SCHED_RR` pattern).

**3. Per-worker CPU affinity**

```python
os.sched_setaffinity(0, {worker_id % cpu_count})
```

Each worker is pinned to a dedicated CPU core, eliminating L2/L3 cache thrashing and TLB flushes caused by process migration across cores.

**4. Pre-warmed buffer pool**

A C++ `buffer_pool` pre-allocates `std::vector<char>` buffers at startup. Per-request allocation becomes an O(1) list pop — no `malloc`/`free` on the critical path.

**5. Route table frozen and COW-shared**

Before forking, `app._sync_routes_to_core()` freezes the radix trie. All workers inherit it as read-only copy-on-write pages — never copied, never locked.

**6. Lazy imports cut startup time by 3–4×**

```python
# Heavy imports (pydantic, openapi, routing) deferred to first use
# `from astraapi import AstraAPI` → ~1-1.5s  (vs ~4.7s eager)
```

---

## 🔌 WebSocket Architecture

```
Client  ──WS Frame──►  C++ ws_frame_parser (ring buffer, RFC 6455)
                            │
                            ▼
                    _WsFastChannel.feed()  ──► Python endpoint await
                            │
                  (echo detected?) ──YES──►  _handle_ws_frames_echo_fd
                            │                 (direct FD write, no Python)
                           NO
                            ▼
                    Python endpoint processes frame
                            │
                    ws.send_text() / send_bytes() / send_json()
                            │
                    C++ ws_build_frame_bytes()  (single allocation)
                            │
                    transport.write()
```

- **Echo auto-detection**: if an endpoint echoes received data, AstraAPI switches to a direct-fd echo handler that bypasses Python entirely
- **Batch frame building**: multiple sends within the same event loop tick are coalesced into a single `transport.write()` via `_ws_build_frames_batch`
- **Backpressure**: pauses transport reading when buffer > 256 messages or 8 MB, resumes at 64 messages / 2 MB
- **WebSocket Groups**: broadcast to N connections using pre-built frames (`_ws_groups.py`) — frame built once, sent to all

---

## 🧩 Project Structure

```
astraapi/
├── __init__.py              # Lazy-import gateway (startup: ~1.5s)
├── applications.py          # AstraAPI class — main app entrypoint
├── routing.py               # APIRouter, APIRoute, path operation decorators
├── _cpp_server.py           # asyncio.Protocol bridge to C++ core
├── _multiworker.py          # Zero-lock multi-worker supervisor
├── _astraapi_core.so        # Compiled C++20 extension
├── _request.py              # Request object
├── _response.py             # Response classes (JSON, HTML, Stream, File…)
├── _routing_base.py         # Base routing primitives
├── _middleware_impl.py      # Middleware stack builder
├── _websocket.py            # WebSocket wrapper (CppWebSocket)
├── _ws_groups.py            # WebSocket group broadcast
├── param_functions.py       # Body, Query, Path, Header, Cookie, Form, File, Depends, Security
├── dependencies/            # DI resolver
├── openapi/                 # OpenAPI schema generation
└── security/                # OAuth2, HTTP Basic/Bearer, API Key

cpp_core/
├── src/
│   ├── app.cpp              # CoreApp, HTTP handler, InlineResult
│   ├── router.cpp           # Radix trie route matching
│   ├── http_parser.cpp      # llhttp wrapper
│   ├── param_extractor.cpp  # Path/query/header param extraction
│   ├── json_writer.cpp      # yyjson-based JSON serialization
│   ├── json_parser.cpp      # yyjson-based JSON parsing
│   ├── ws_frame_parser.cpp  # RFC 6455 WebSocket frame parser
│   ├── ws_ring_buffer.cpp   # Ring buffer for WS frames
│   ├── middleware_engine.cpp # CORS, TrustedHost in C++
│   ├── body_parser.cpp      # JSON/form body parsing
│   ├── response_pipeline.cpp# Response assembly
│   └── dependency_resolver.cpp # C++ DI graph resolution
├── third_party/
│   ├── llhttp/              # Node.js HTTP parser
│   ├── yyjson/              # Fast JSON (SIMD)
│   └── ryu/                 # Fast float→string (d2s)
└── CMakeLists.txt           # C++20, -O3 -march=native -flto
```

---

## ⚙️ Configuration

```python
app = AstraAPI(
    title="My API",
    version="1.0.0",
    description="API description (Markdown supported)",
    docs_url="/docs",          # Swagger UI
    redoc_url="/redoc",        # ReDoc
    openapi_url="/openapi.json",
    root_path="/api/v1",       # For reverse proxy setups
)

app.run(
    host="0.0.0.0",
    port=8000,
    workers=4,         # Number of worker processes
    reload=False,      # Enable hot reload (dev only)
)
```

---

## 📚 Dependencies

| Package | Role |
|---------|------|
| `pydantic ≥ 2.7` | Request/response validation and serialization |
| `uvloop` (Linux/macOS) | High-performance asyncio event loop |
| `winloop` (Windows) | High-performance asyncio event loop for Windows |
| `orjson` | Fast JSON fallback for Python-side serialization |
| `watchfiles` | File change detection for hot reload |
| `annotated-doc` | Rich type annotation documentation |

Optional (install via `pip install astraapi[standard]`):
`httpx`, `jinja2`, `python-multipart`, `email-validator`, `pydantic-settings`, `pydantic-extra-types`

---

## 🤝 Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to set up the development environment, build the C++ core, and run the test suite.

---

## 📄 License

MIT License — see [LICENSE](LICENSE).

---

<p align="center">
  Made with ⚡ by <a href="https://github.com/Lumos-Labs-HQ">Lumos Labs HQ</a> · Inspired by <a href="https://fastapi.tiangolo.com">FastAPI</a>
</p>
