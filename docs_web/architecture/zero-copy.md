# Zero-Copy and Caching

AstraAPI minimizes data copying at every layer of the stack. This page explains the specific techniques and their performance impact.

## Zero-Copy HTTP Parsing with llhttp

### The Problem

A typical HTTP parser allocates Python strings for every header, the method, the path, and the query string:

```python
# What pure Python does
method = buf[method_start:method_end].decode()  # Copy + decode
path = buf[path_start:path_end].decode()        # Copy + decode
headers = {}
for h in header_lines:
    key = h[:colon].decode().lower()            # Copy + decode + lower()
    val = h[colon+1:].decode().strip()          # Copy + decode + strip()
    headers[key] = val
```

For a request with 20 headers, this creates **40+ Python string objects** before your endpoint even runs.

### AstraAPI's Solution

The llhttp parser operates directly on the raw buffer via callbacks:

```cpp
struct ParsedHttpRequest {
    StringView method;       // Pointer + length into original buffer
    StringView path;         // Pointer + length into original buffer
    StringView query_string; // Pointer + length into original buffer
    Header headers[32];      // All point into original buffer
};
```

No copies are made during parsing. Headers are only converted to Python strings when (and if) the endpoint accesses them.

### Lazy Header Materialization

```cpp
PyObject* get_header(ParsedHttpRequest* req, const char* name) {
    for (int i = 0; i < req->header_count; i++) {
        if (strncasecmp(req->headers[i].key.data, name, 
                        req->headers[i].key.len) == 0) {
            // Only now create a Python string
            return PyUnicode_FromStringAndSize(
                req->headers[i].value.data,
                req->headers[i].value.len
            );
        }
    }
    Py_RETURN_NONE;
}
```

Most endpoints only access a few headers (`content-type`, `authorization`). The rest stay as zero-cost pointers.

## Response Cache

### Design

```cpp
#define RESPONSE_CACHE_SLOTS 8

struct CachedResponse {
    uint64_t content_hash;    // FNV-1a hash of serialized content
    PyObject* response_bytes; // Complete HTTP response (headers + body)
    int status_code;
    bool keep_alive;
    bool is_head;
};

static CachedResponse s_dict_cache[RESPONSE_CACHE_SLOTS];
static CachedResponse s_response_cache[RESPONSE_CACHE_SLOTS];
```

The cache is:
- **Content-addressed** — FNV-1a hash of the actual JSON content, not the object pointer
- **GC-safe** — does not depend on object identity (objects can move during GC)
- **Small and fast** — 8 slots, O(1) lookup via modulo hash

### Cacheable Content

Only hashable, JSON-serializable content is cached:

| Type | Cached? | Reason |
|------|---------|--------|
| `dict` with str/int/float/bool/None values | Yes | Deterministic hash |
| `list` of primitives | Yes | Deterministic hash |
| `dict` with nested dicts/lists | Yes | Recursive hash |
| `dict` with unhashable values | No | Cannot compute stable hash |
| `Response` object | Yes | Hash of body + headers |
| `StreamingResponse` | No | Body not known upfront |

### Cache Hit Path

```cpp
uint64_t hash = hash_dict_content(result);
int slot = hash % RESPONSE_CACHE_SLOTS;

if (s_dict_cache[slot].content_hash == hash) {
    // Cache hit! Return pre-built response
    write_response_direct(sock_fd, transport, 
                          s_dict_cache[slot].response_bytes);
    return;
}

// Cache miss: build response
PyObject* resp_bytes = build_json_response_bytes(result, status, keep_alive);
s_dict_cache[slot].content_hash = hash;
Py_XDECREF(s_dict_cache[slot].response_bytes);
s_dict_cache[slot].response_bytes = resp_bytes;  // Steals reference
write_response_direct(sock_fd, transport, resp_bytes);
```

### Performance Impact

For a health check endpoint returning `{"status": "ok"}`:

| Path | Time |
|------|------|
| Cache miss (serialize + build) | ~5-10us |
| Cache hit (hash + lookup + write) | ~200ns |
| **Speedup** | **25-50x** |

## Pre-Built Error Responses

At module initialization, AstraAPI builds complete HTTP responses for common errors:

```cpp
// Built once, used millions of times
s_resp_404 = PyBytes_FromString(
    "HTTP/1.1 404 Not Found\r\n"
    "content-length: 0\r\n"
    "connection: close\r\n\r\n"
);

s_resp_405 = PyBytes_FromString(
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "content-length: 0\r\n"
    "connection: close\r\n\r\n"
);
```

When a 404 occurs with no CORS headers needed, the response is served by writing the pre-built `PyBytes` — **zero allocation** on the error path.

## Direct Socket Write (write_response_direct)

### The Problem

Writing a response in Python:

```python
# This happens on EVERY response
transport.write(data)
# Internally:
# 1. transport.__getattribute__('write')  -- dict lookup
# 2. method.__call__(data)                -- generic Python call
```

### The Solution

For small responses (<= 16KB), AstraAPI bypasses Python's transport entirely:

```cpp
int write_response_direct(int fd, PyObject* transport, PyObject* data) {
    if (fd >= 0 && PyBytes_Check(data)) {
        Py_ssize_t len = PyBytes_GET_SIZE(data);
        if (len > 0 && len <= 16384) {
            const char* ptr = PyBytes_AS_STRING(data);
            ssize_t sent = platform_socket_write(fd, ptr, (size_t)len);
            if (sent == len) {
                return 0;  // Full write, zero Python overhead!
            }
            // Partial write or EAGAIN: fall through
        }
    }
    // Fallback to Python transport
    return write_to_transport(transport, data);
}
```

### Why It Matters

- `PyObject_CallMethodOneArg("write")` costs **~700ns** (string hash + dict lookup + generic call)
- `send()` syscall costs **~150ns** (direct kernel call)
- At 200K req/s: **110ms of CPU time saved per second**

## WebSocket Direct FD writev()

For WebSocket responses, AstraAPI can bypass Python's transport entirely using `writev()`:

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
- `v2` variant buffers unsent data on `EAGAIN`

## Adaptive Buffer Limits

At different concurrency levels, different buffer strategies are optimal:

```
Low concurrency (< 100 conns):
  Write buffer: 256KB high / 64KB low
  -> Batches many small writes into fewer syscalls

High concurrency (> 5000 conns):
  Write buffer: 16KB high / 4KB low
  -> Prevents 10K x 256KB = 2.5GB aggregate buffer bloat
```

The switch is automatic based on active connection count.

## JSON Serialization Optimizations

### yyjson vs Python json

AstraAPI uses **yyjson** for parsing — ~3GB/s parse speed with 2-phase GIL release.

### Custom Streaming Writer

For serialization, a custom streaming writer outperforms generic JSON libraries:

| Operation | Time |
|-----------|------|
| `json.dumps()` (small dict) | ~3us |
| `orjson.dumps()` (small dict) | ~800ns |
| **AstraAPI C++ writer** | **~400ns** |

### SIMD String Escape

For strings with no special characters, the serializer uses `memcpy` instead of character-by-character escaping:

```cpp
// AVX2: check 32 bytes at once for special chars
__m256i chunk = _mm256_loadu_si256((__m256i*)ptr);
__m256i quote_mask = _mm256_cmpeq_epi8(chunk, quote_vec);
if (_mm256_testz_si256(quote_mask, quote_mask)) {
    // Fast path: no escaping needed, bulk copy
    writer.append(ptr, 32);
} else {
    // Slow path: scalar escape
    writer.escape_string(ptr, len);
}
```

Most API responses (names, IDs, statuses) are plain ASCII and hit the fast path.

### Ryu Float Formatting

Floats are formatted with **ryu** — exact shortest decimal representation in ~50ns:

```cpp
char buf[32];
int len = d2s(val, buf);  // No printf overhead, no rounding errors
```

### Direct Bytes Creation

The serializer writes JSON directly into a pooled `std::vector<char>`, then creates a single `PyBytes` object. No intermediate Python string is created.

```cpp
std::vector<char>* buf = acquire_buffer(estimated_size);
// ... write JSON into buf ...
PyObject* bytes = PyBytes_FromStringAndSize(buf->data(), buf->size());
release_all_buffers();
```

## Buffer Pool

Thread-local `std::vector<char>` buffers eliminate per-request allocation:

```cpp
thread_local std::vector<std::vector<char>> buffer_pool;
thread_local size_t buffer_pool_next = 0;
```

- Max 32 buffers per thread
- 8KB initial capacity, grows for large responses
- `release_all_buffers()` resets the index — no individual free needed
- Pre-warmed at startup to avoid cold-start allocation
