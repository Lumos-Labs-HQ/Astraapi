# HTTP Pipeline

Understanding the exact path a request takes through AstraAPI explains both its speed and its compatibility.

## Complete Request Lifecycle

```
┌─────────────┐
│   Client    │
└──────┬──────┘
       │ HTTP/1.1 request
       ▼
┌─────────────────────────────────────────┐
│  Kernel TCP buffer                       │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────┐
│  asyncio selector / epoll                │
│  (woken up by kernel)                    │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────┐
│  CppHttpProtocol.data_received(data)     │
│  Python asyncio Protocol                 │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────┐
│  C++ core: handle_http_append_and_dispatch│
│  1. Fast-path GET/HEAD scan              │
│  2. llhttp parse (zero-copy)             │
│  3. Match route: static hash → radix trie│
│  4. Extract path/query/header/cookie params│
│  5. Body parse: yyjson / multipart / form │
│  6. Call Python endpoint                 │
│  7. Serialize response: custom writer    │
│  8. Compress: libdeflate / brotli        │
└─────────────────────────────────────────┘
       │
       ├────────────────────────┐
       │ Sync endpoint          │ Async endpoint (PYGEN_RETURN)
       ▼                        ▼
┌─────────────┐          ┌─────────────────────┐
│ C++ receives│          │ C++ receives result │
│ result      │          │ immediately         │
└──────┬──────┘          └──────────┬──────────┘
       │                            │
       ▼                            ▼
┌─────────────────────────────────────────┐
│  C++: serialize response                 │
│  1. Check response cache (FNV-1a hash)   │
│  2. Cache miss → custom SIMD serializer  │
│  3. Build HTTP response bytes            │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────┐
│  C++: write_response_direct()            │
│  POSIX send() on socket fd for small     │
│  responses (zero Python overhead)        │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────┐
│  Kernel send()                           │
└─────────────────────────────────────────┘
       │
       ▼
┌─────────────┐
│   Client    │
└─────────────┘
```

## Async Endpoint (PYGEN_NEXT Path)

When an endpoint actually suspends (e.g., awaits a database query):

```
C++ core calls PyIter_Send(coro, None, &yielded)
        │
        │ PYGEN_NEXT (coroutine suspended)
        ▼
┌─────────────────────────────────────────┐
│  C++ returns tuple to Python:            │
│  ("async", coro, yielded_future, status, │
│   keep_alive)                            │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  Python: create_task(_handle_async())    │
│  Task is scheduled on asyncio event loop │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  Event loop runs other connections       │
│  (GIL released, high concurrency)        │
└─────────────────────────────────────────┘
        │
        │ Database query completes
        │ (asyncio wakes the task)
        ▼
┌─────────────────────────────────────────┐
│  Python: _drive_coro() resumes           │
│  Coroutine completes with result         │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  C++: write_async_result()              │
│  Serializes and writes response          │
│  (same fast path as sync)                │
└─────────────────────────────────────────┘
```

## The Fast Path

For the common case — a sync endpoint or a trivial async endpoint that returns immediately:

```python
@app.get("/health")
def health():
    return {"status": "ok"}
```

The request **never leaves C++** after the endpoint call:

| Step | Time | Where |
|------|------|-------|
| Fast-path GET scan | ~100ns | C++ |
| Route match (static hash) | ~50ns | C++ |
| Call endpoint | ~1μs | Python (GIL held) |
| Serialize response | ~200ns | C++ (cache hit) |
| write_response_direct | ~150ns | C++ (send()) |
| **Total** | **~1.5μs** | **~666K req/s theoretical max** |

In practice, Python overhead, kernel networking, and event loop scheduling bring this to **~200-250K req/s** on modern hardware.

## Buffer Management

### Read Buffer

Each connection maintains an `_http_buf` managed by C++. Data is appended as it arrives from the kernel. llhttp consumes bytes from the front and reports `total_consumed`. Unconsumed bytes (partial requests) remain in the buffer.

### Write Buffer

Python's `asyncio.Transport` manages the write buffer. AstraAPI sets:

```python
transport.set_write_buffer_limits(high=262144, low=65536)
```

- **256KB high water mark** — batches many small writes into fewer syscalls
- **64KB low water mark** — resumes writing when buffer drains

For WebSocket connections, limits are larger (2MB / 512KB) to handle burst frame writes.

### C++ Buffer Pool

Thread-local `std::vector<char>` buffers are reused from a pool (max 32 buffers × 8KB). This eliminates per-request heap allocation for response building.

## Error Handling

Errors can occur at any stage:

| Stage | Error Type | Handling |
|-------|-----------|----------|
| Parse (llhttp) | Invalid HTTP | 400 Bad Request, close connection |
| Fast-path scan | Malformed request | Fallback to llhttp |
| Route | No match | 404 Not Found (pre-built response) |
| Route | Method not allowed | 405 Method Not Allowed (pre-built) |
| Endpoint | Exception | Python exception handler → 500 |
| Endpoint | ValidationError | 422 Unprocessable Entity |
| Serialization | Non-serializable | 500 Internal Server Error |

All common error responses (404, 405, 500 without CORS) are **pre-built as `PyBytes`** at module initialization — zero allocation on error paths.

## Compression Pipeline

```
Endpoint returns dict/list
        │
        ▼
┌─────────────────────────────────────────┐
│  C++: serialize to JSON bytes            │
│  (custom SIMD writer + ryu floats)       │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  Check Accept-Encoding header            │
│  Client supports gzip/brotli?            │
└─────────────────────────────────────────┘
        │ Yes
        ▼
┌─────────────────────────────────────────┐
│  Compress response                       │
│  libdeflate (2-3x faster than zlib)     │
│  OR brotli (if available)                │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  Build HTTP response with                │
│  Content-Encoding header                 │
└─────────────────────────────────────────┘
```

Compression only applies when:
- Response size exceeds `minimum_size` threshold
- Client sends `Accept-Encoding: gzip` or `br`
- Content is compressible (not images, already compressed)
