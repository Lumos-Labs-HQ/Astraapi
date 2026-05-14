# C++ Core

The C++ core is the engine that makes AstraAPI fast. Written in C++20 and compiled to a Python extension module (`_astraapi_core.so`), it handles everything from HTTP parsing to response serialization using battle-tested libraries and SIMD optimizations.

## Module Structure

```cpp
// cpp_core/src/
app.cpp                   // Main dispatcher: dispatch_one_request (~2800 lines)
router.cpp / router.hpp   // Two-phase router: static hash + radix trie
http_parser.cpp           // llhttp wrapper, ParsedHttpRequest, zero-copy headers
json_parser.cpp           // yyjson wrapper, 2-phase GIL-release parse
json_writer.cpp           // Streaming JSON serializer with SIMD string escape
json_encoder.cpp          // Pydantic-aware recursive encoder
buffer_pool.cpp           // Thread-local vector<char> reuse
body_parser.cpp           // Python-exposed JSON/form/multipart wrappers
middleware_engine.cpp     // Compression: libdeflate gzip, brotli
request_pipeline.cpp      // Query/header/cookie parsing into single PyDict
response_pipeline.cpp     // encode_to_json_bytes fast path
param_extractor.cpp       // O(1) batch param extraction
security.cpp              // Bearer/Basic auth parsing
form_parser.cpp           // URL-encoded and multipart body parsing
streaming_multipart.cpp   // RFC 2046 streaming multipart with Boyer-Moore-Horspool
error_response.cpp        // Error list serialization
openapi_gen.cpp           // OpenAPI schema serialization
websocket_handler.cpp     // WebSocket JSON parse/serialize helpers
ws_frame_parser.cpp       // RFC 6455 frame parser/builder, SIMD unmasking
ws_ring_buffer.cpp        // Ring buffer, slab allocator, direct FD writev()
asgi_constants.cpp        // Pre-interned ASGI strings, pre-built responses
utils.cpp                 // URL percent-decode, query parse, header normalization
module.cpp                // Module entry point, method table (~40 exports)
```

## HTTP Parser: llhttp

AstraAPI uses **llhttp** — the same battle-tested HTTP/1.1 parser from Node.js.

### Fast-Path GET/HEAD Scanner

Before invoking the full llhttp state machine, a hand-rolled scanner checks if the request is a simple `GET /path HTTP/1.1` or `HEAD`. If so, it parses method, URI, and headers inline — **~2-3× faster** for the common case.

```cpp
// Pseudo-code of the fast path
if (is_simple_get_or_head(buf, len)) {
    // Inline parse: no llhttp state machine overhead
    req.method = "GET";
    req.path = extract_path(buf);
    req.headers = extract_headers_fast(buf);
} else {
    // Full llhttp state machine for complex requests
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    llhttp_execute(&parser, buf, len);
}
```

### Zero-Copy Header Parsing

llhttp callbacks receive `const char* + length` pointers into the raw input buffer. No copies are made during parsing:

```cpp
struct ParsedHttpRequest {
    StringView method;      // Points into raw buffer
    StringView path;        // Points into raw buffer
    StringView query_string;// Points into raw buffer
    Header headers[MAX_HEADERS]; // All point into raw buffer
};
```

Headers are only converted to Python strings when (and if) the endpoint accesses them.

### Optimizations

- **Thread-local parser instance** — avoids per-request `llhttp_init()` overhead
- **F_SKIPBODY** — for GET/HEAD/OPTIONS, tells llhttp not to wait for body bytes
- **Chunked body reassembly** — thread-local `std::vector<char>` (max 10MB)

## JSON Parser: yyjson

AstraAPI uses **yyjson** for JSON parsing — a high-performance JSON library claiming ~3GB/s parse speed.

### 2-Phase GIL-Release API

```cpp
// Phase 1: Parse raw bytes (NO GIL held — pure C)
yyjson_doc* doc = yyjson_parse_raw(buf, len, ...);

// Phase 2: Convert to Python objects (GIL held)
PyObject* py_dict = yyjson_doc_to_pyobject(doc);
```

This means during JSON body parsing, the GIL is **completely released**, allowing other Python threads to run.

### Merge-to-Dict Optimization

For `embed_body_fields` scenarios, yyjson can merge top-level JSON keys directly into an existing `PyDict` without creating an intermediate dict:

```cpp
yyjson_doc_merge_to_dict(doc, existing_dict);
```

## JSON Serializer: Custom Streaming Writer

The serializer is **not** yyjson's writer — it's a custom streaming implementation optimized for Python object graphs.

### SIMD-Accelerated String Escaping

```cpp
// AVX2 path: process 32 bytes at once
__m256i chunk = _mm256_loadu_si256((__m256i*)ptr);
__m256i mask = _mm256_cmpeq_epi8(chunk, quote_vec);
if (_mm256_testz_si256(mask, mask)) {
    // No special chars — fast copy
} else {
    // Scalar fallback for this chunk
}
```

- **AVX2**: 32 bytes at a time
- **SSE2**: 16 bytes at a time
- **Scalar**: fallback for remaining bytes

### Ryu Float Formatting

Floats are formatted using **ryu** — the same algorithm used by Go and Rust — producing the exact shortest decimal representation:

```cpp
// ryu: exact shortest representation, no printf overhead
char buf[32];
int len = d2s(val, buf);  // ~50ns per float
```

### Special Type Handling

The serializer natively handles Python types without intermediate conversion:

| Type | Handling |
|------|----------|
| `datetime` | `isoformat()` call, then serialize |
| `Decimal` | `str()` conversion, then serialize |
| `UUID` | `hex` format |
| `Enum` | `value` extraction |
| `BaseModel` | `model_dump()` then recurse |
| `dataclass` | `asdict()` then recurse |
| `PurePath` | `str()` conversion |

### Buffer Pool

The serializer writes into thread-local `std::vector<char>` buffers from a pool (max 32 buffers × 8KB). This eliminates per-request heap allocation for response building.

## Router: Two-Phase Design

Inspired by Hono.js, the router uses a two-phase lookup:

### Phase A — Static Routes (O(1))

```cpp
std::unordered_map<std::string, int, SVHash, SVEqual> static_routes_;
```

- Key is the full pattern string (e.g., `"/api/users"`)
- Transparent `string_view` hash/equality — **zero heap allocation** on lookup
- Most API routes are static and hit this path

### Phase B — Parametric Routes (Radix Trie)

- Only patterns containing `{param}` go into the trie
- First-byte dispatch array (size 128) at each node for fast child lookup
- Supports `/{param}` (greedy until `/`) and `/{param:path}` (catch-all)
- **Zero-allocation matching**: `string_view` into request buffer with backtracking

```cpp
auto it = static_routes_.find(path_sv);
if (it != static_routes_.end()) {
    return MatchParams{it->second};  // O(1) hit
}
return match_recursive(root_, path, len, 0, result);  // Trie walk
```

## Compression: libdeflate

AstraAPI uses **libdeflate** for gzip compression — 2-3× faster than zlib:

```cpp
#ifdef HAS_LIBDEFLATE
    // libdeflate path: single-shot, optimized
    compressed = libdeflate_gzip_compress(level, input, input_len);
#else
    // zlib fallback
    compressed = zlib_gzip_compress(level, input, input_len);
#endif
```

Optional **brotli** support is also available when `brotlienc`/`brotlidec` are installed on the system.

## WebSocket: Direct FD writev()

For WebSocket responses, AstraAPI can bypass Python's transport entirely:

```cpp
// Build iovec: [frame_header_scratch, payload_from_ring_buffer]
struct iovec iov[2] = {
    {frame_header, header_len},
    {payload_ptr, payload_len}
};

Py_BEGIN_ALLOW_THREADS;
platform_socket_writev(fd, iov, 2);
Py_END_ALLOW_THREADS;
```

- **ZERO payload copy**: payload stays in the ring buffer
- GIL released during the syscall
- `v2` variant buffers unsent data on `EAGAIN` instead of falling back to Python

## WebSocket SIMD Unmasking

Payload unmasking uses SIMD for bulk XOR operations:

```cpp
// AVX2: unmask 32 bytes at a time
__m256i mask = _mm256_set1_epi32(*(uint32_t*)mask_key);
_mm256_storeu_si256((__m256i*)out, _mm256_xor_si256(data, mask));
```

- AVX2 (32 bytes), SSE2 (16 bytes), NEON (ARM), or scalar fallback

## Endpoint Dispatcher

The heart of the core is `dispatch_one_request` (~2800 lines in `app.cpp`):

### Sync Endpoint Path

```cpp
PyObject* result = PyObject_Call(endpoint, args, kwargs);
// ... serialize result ...
write_response_direct(sock_fd, transport, response_bytes);
```

### Async Endpoint Path

```cpp
PyRef coro(PyObject_Call(endpoint, args, kwargs));
PyObject* yielded;
PySendResult status = PyIter_Send(coro.get(), Py_None, &yielded);

if (status == PYGEN_RETURN) {
    // Completed synchronously — fast path!
    // Serialize and write directly from C++
} else if (status == PYGEN_NEXT) {
    // Suspended — hand off to Python asyncio
    // Return tuple: ("async", coro, yielded, status_code, keep_alive)
    PyObject_SetAttr(yielded, s_fut_blocking, Py_False);
}
```

The **PYGEN_RETURN fast path** is critical: for async endpoints that complete without awaiting (e.g., `async def` that just returns a dict), AstraAPI skips Python's `create_task` + `_handle_async` overhead entirely.

## Transport Write: Multiple Layers

From fastest to slowest:

1. **`write_response_direct`** — POSIX `send()` on socket fd for small responses (≤16KB). Zero Python overhead on full write.
2. **`write_to_transport`** — Calls cached `transport.write` bound method directly (skips `PyObject_CallMethodOneArg` string lookup).
3. **`serialize_json_and_write_response`** — Fused path: serialize → build header → single `PyBytes` alloc → write.
4. **`build_and_write_http_response`** — Ultra-fast path for cached JSON prefix + no CORS + no compression.

## Build System

**CMake 3.20+** with **scikit-build-core**:

```cmake
# Key flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -flto -fno-rtti -fvisibility=hidden")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")  # Except cross-compile
```

**Vendored C sources:**
- `third_party/yyjson/yyjson.c`
- `third_party/ryu/d2s.c`
- `third_party/llhttp/llhttp.c`, `api.c`, `http.c`

**Optional system libraries (auto-detected):**
- `libdeflate` — fast gzip
- `brotlienc` / `brotlidec` — Brotli compression
- `zlib` — deflate/gzip fallback

## Memory Safety

All C++ code follows strict reference counting discipline:

- `PyRef` RAII class ensures `Py_DECREF` on all paths
- Cached Python objects cleared in `module_free`
- No `Py_AtExit` callbacks — cleanup tied to module lifetime
- GIL held during all Python C API calls
