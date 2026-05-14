# Memory Model

AstraAPI manages memory across two runtimes: Python (with its garbage collector and reference counting) and C++ (with manual memory management). Getting this right is critical for both performance and stability.

## Reference Counting in C++

Every `PyObject*` in the C++ core is wrapped in a `PyRef` RAII class:

```cpp
class PyRef {
    PyObject* ptr;
public:
    explicit PyRef(PyObject* p) : ptr(p) {}
    ~PyRef() { Py_XDECREF(ptr); }
    
    PyRef(const PyRef&) = delete;
    PyRef& operator=(const PyRef&) = delete;
    
    PyRef(PyRef&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    PyRef& operator=(PyRef&& other) noexcept {
        if (this != &other) {
            Py_XDECREF(ptr);
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }
    
    PyObject* get() const { return ptr; }
    PyObject* release() {
        PyObject* tmp = ptr;
        ptr = nullptr;
        return tmp;
    }
};
```

This guarantees:
- No leaks: every `PyRef` destructor calls `Py_XDECREF`
- No double-free: move semantics transfer ownership
- No accidental copies: copy constructor is deleted

## Cached Static Objects

The C++ core caches frequently-used Python objects to avoid recreation:

```cpp
static PyObject* s_dict_cache[RESPONSE_CACHE_SLOTS];
static PyObject* s_response_cache[RESPONSE_CACHE_SLOTS];
static PyObject* s_async_tag;
static PyObject* s_fut_blocking;
static PyObject* s_empty_tuple;
```

These are:
- Created once during module initialization
- Shared across all requests (read-only after init)
- Cleared in `module_free()` when the module is unloaded

## Pre-Built Response Objects

At module init, AstraAPI pre-builds common error responses as complete `PyBytes` objects:

```cpp
// Pre-built in asgi_constants.cpp
g_500_start  // "HTTP/1.1 500 Internal Server Error\r\n..."
g_500_body   // JSON error body
s_resp_404   // Complete 404 response (when no CORS)
s_resp_405   // Complete 405 response (when no CORS)
s_resp_500   // Complete 500 response (when no CORS)
```

When a 404 occurs with no CORS headers needed, the response is served by simply writing the pre-built `PyBytes` — **zero allocation**.

## The GIL

AstraAPI follows strict GIL discipline:

### GIL Must Be Held

- `PyObject_Call`, `PyObject_CallNoArgs`
- `PyIter_Send`
- `PyDict_GetItem`, `PyList_GetItem`
- `Py_DECREF`, `Py_XDECREF`
- `PyTuple_Pack`, `PyBytes_FromString`

### GIL Released Automatically

- `yyjson_parse_raw` (Phase 1 JSON parsing — pure C)
- Socket I/O via `platform_socket_write` / `writev`
- Background thread operations
- `await` in Python coroutines

### Critical Section Pattern

```cpp
PyGILState_STATE gstate = PyGILState_Ensure();
// ... Python C API calls ...
PyGILState_Release(gstate);
```

AstraAPI's C++ module is always called from Python-held contexts (asyncio Protocol methods), so explicit GIL state management is rarely needed.

## Object Lifetimes

### Per-Request Objects

Created and destroyed within a single request:

- `ParsedHttpRequest` struct (stack allocated)
- `PyRef` wrappers for kwargs, results
- Response bytes (refcount = 1, handed to transport)

### Per-Connection Objects

Live as long as the TCP connection:

- `CppHttpProtocol` instance (pooled, reused)
- `_http_buf` (C++ managed)
- Cached `_transport_write` method
- WebSocket ring buffer capsule (pooled)

### Global Objects

Live for the process lifetime:

- Route table (frozen after startup)
- Response cache slots
- Cached Python strings (`s_async_tag`, etc.)
- Pre-built status lines (all 600 codes)
- Pre-built JSON prefixes (all 600 codes)
- Buffer pool vectors

## Buffer Pool

Thread-local `std::vector<char>` buffers are pooled to eliminate per-request allocation:

```cpp
// buffer_pool.cpp
thread_local std::vector<std::vector<char>> buffer_pool;
thread_local size_t buffer_pool_next = 0;

std::vector<char>* acquire_buffer(size_t min_size) {
    if (buffer_pool_next < buffer_pool.size()) {
        auto* buf = &buffer_pool[buffer_pool_next++];
        if (buf->capacity() < min_size) buf->reserve(min_size);
        buf->clear();
        return buf;
    }
    // Create new buffer
    buffer_pool.emplace_back();
    buffer_pool.back().reserve(std::max(min_size, (size_t)8192));
    return &buffer_pool.back();
}

void release_all_buffers() {
    buffer_pool_next = 0;
}
```

- Max 32 buffers per thread
- 8KB initial capacity
- Grows as needed for large responses
- `release_all_buffers()` resets the index — no individual free needed

## GC Interaction

AstraAPI takes control of Python's garbage collector for stable performance:

```python
def run_server(self, ...):
    gc.disable()       # Disable automatic cyclic GC
    gc.collect(0)      # Clear young generation
    gc.collect(1)      # Clear middle generation
    gc.freeze()        # Move all remaining objects to permanent generation
```

### Why This Works

1. **Reference counting handles most cleanup** — objects are freed immediately when refcount hits zero
2. **No cycles in request path** — request processing does not create reference cycles
3. **Frozen generation is never scanned** — eliminates GC pause sources
4. **Safe for long-running processes** — if cycles do form, `gc.collect()` can be called manually

### Protocol Object Pooling

Instead of allocating new protocol objects for every connection:

```python
# Pre-warm at startup
for _ in range(prewarm_count):
    pool.release(CppHttpProtocol(...))

# Reuse at runtime
proto = pool.acquire(...)
# ... connection lifetime ...
pool.release(proto)
```

This eliminates:
- `__new__` allocation
- `__slots__` initialization
- GC tracking registration

## Memory Leak Prevention

AstraAPI includes several safeguards against memory leaks:

### Connection Limits

```python
if len(active_connections) > max_connections:
    transport.close()  # Reject new connections
```

### Pending Task Cleanup

```python
def connection_lost(self, exc):
    for task in self._pending_tasks:
        task.cancel()
    self._pending_tasks.clear()
```

### Pool Trimming

```python
async def _pool_trim(self):
    while True:
        await asyncio.sleep(120)
        target = max(64, len(active_connections) // 2)
        while len(pool) > target:
            pool.popleft()
        gc.collect(0)
```

### Module Cleanup

```cpp
static void module_free(void* module) {
    cleanup_cached_refs();      // Clear cached PyObjects
    json_writer_cleanup();      // Clear writer caches
    cleanup_asgi_constants();   // Clear ASGI cached strings
    cleanup_param_registry();   // Clear route param caches
}
```

## Memory Footprint

Typical memory usage per connection:

| Component | Memory |
|-----------|--------|
| `CppHttpProtocol` (with `__slots__`) | ~200 bytes |
| `_http_buf` (empty) | ~64 bytes overhead |
| C++ buffer pool (per thread) | ~256KB shared |
| Write buffer (average) | ~4KB |
| Kernel socket buffers | ~8KB |
| **Total per connection** | **~12KB** |

At 10,000 concurrent connections: **~120MB** — well within the capabilities of a single process.

## WebSocket Ring Buffer

WebSocket connections use a ring buffer for frame handling:

```cpp
struct WsRingBuffer {
    char* data;
    size_t capacity;
    size_t head;  // Write position
    size_t tail;  // Read position
};
```

- Slab allocator for frame metadata
- Payload stays in ring buffer during `writev()` — **zero copy**
- Capsules are pooled and reused across connections
