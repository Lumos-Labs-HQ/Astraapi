# Python Asyncio Bridge

AstraAPI's C++ core doesn't replace Python's asyncio — it **cooperates** with it. The Python asyncio Protocol (`CppHttpProtocol`) is the bridge that connects the C++ core to Python's event loop.

## Design Philosophy

> C++ does what C++ does best: parse, route, serialize.  
> Python does what Python does best: run your business logic, manage async state.

## The Protocol Class

```python
class CppHttpProtocol(asyncio.Protocol):
    __slots__ = (
        "_core", "_transport", "_http_buf", "_ka_deadline",
        "_ka_timeout", "_loop", "_wr_paused", "_ws",
        "_ws_handler", "_ws_ring_buf", "_ws_fd",
        "_ws_ping_handle", "_ws_pong_received", "_ws_task",
        "_connections_set", "_pending_tasks",
        "_pending_tasks_discard", "_active_count", "_sock_fd",
        "_core_batch", "_core_append_dispatch",
        "_loop_create_task", "_ka_needs_reset",
        "_needs_req_ctx", "_transport_write"
    )
```

Using `__slots__` saves **~60 bytes per connection** compared to a regular class — significant when managing 10K+ concurrent connections.

## Connection Lifecycle

### `connection_made(transport)`

```python
def connection_made(self, transport):
    self._transport = transport
    self._transport_write = transport.write  # Cache write method
    
    # TCP optimizations
    sock = transport.get_extra_info('socket')
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
    
    # Write buffer limits
    transport.set_write_buffer_limits(high=262144, low=65536)
```

Key optimizations:
- **`TCP_NODELAY`** — disables Nagle's algorithm for low-latency responses
- **`TCP_QUICKACK`** — re-armed per read to reduce ACK latency (Linux)
- **`_transport_write` caching** — avoids `PyObject_CallMethodOneArg("write")` on every response

### `data_received(data)`

```python
def data_received(self, data):
    # Append to HTTP buffer and dispatch via C++
    result = self._core_append_dispatch(
        self._http_buf,
        data,
        self._transport_write,
        self._sock_fd
    )
    
    # result is either:
    # - int (sync handled, response written from C++)
    # - tuple ("async", coro, yielded, status, keep_alive)
    # - tuple ("ws", endpoint, ...)
    # - InlineResult (needs Pydantic validation fallback)
    
    if isinstance(result, tuple) and result[0] == "async":
        self._loop_create_task(self._handle_async(result))
```

For **sync endpoints**, the response is written entirely from C++ and Python only sees an `int` return code.

## Async Handling

When an endpoint suspends (does actual async I/O like database calls), the C++ core returns a tuple and Python takes over:

```python
async def _handle_async(self, async_info):
    tag, coro, yielded, status_code, keep_alive = async_info
    
    try:
        result = await coro
    except Exception as e:
        result = e
    
    # Write result back via C++
    if isinstance(result, (dict, list)):
        self._core.write_async_result(
            self._transport_write,
            result,
            status_code,
            keep_alive
        )
    else:
        self._build_response_from_any(result)
```

### Why This Design?

1. **Fast path stays in C++** — trivial async endpoints (no awaits) complete via `PYGEN_RETURN` without ever entering `_handle_async`
2. **Real async works naturally** — when a coroutine suspends, Python's event loop handles the resumption exactly as it should
3. **Zero GIL contention** — while a coroutine awaits I/O, the GIL is released and other connections can be processed

## Batch Keep-Alive (No Per-Connection Timers)

AstraAPI does **not** use per-connection `call_later` callbacks for keep-alive. Instead, it uses a **batch sweep** every 10 seconds:

```python
async def _ka_sweep(self):
    while True:
        await asyncio.sleep(10)
        now = time.monotonic()
        
        for conn in active_connections:
            if conn._ka_needs_reset:
                conn._ka_needs_reset = False
                conn._ka_deadline = now + conn._ka_timeout
            elif conn._ka_deadline > 0 and now > conn._ka_deadline:
                conn._transport.close()
```

This means `_monotonic()` is called **once per sweep** (every 10s), not once per request. At 200K req/s, this eliminates **~20,000 timer callbacks per second**.

## Protocol Object Pooling

At high concurrency, allocating a new `CppHttpProtocol` per connection is expensive. AstraAPI maintains a pool:

```python
class _ProtocolPool:
    def __init__(self, maxsize=32768):
        self._pool = collections.deque(maxlen=maxsize)
    
    def acquire(self, core_app, loop, ka_timeout, connections_set):
        try:
            proto = self._pool.pop()
        except IndexError:
            return CppHttpProtocol(core_app, loop, ka_timeout, connections_set)
        proto._reinit(core_app, loop, ka_timeout, connections_set)
        return proto
    
    def release(self, proto):
        self._pool.append(proto)
```

Pre-warming at startup eliminates allocation overhead during connection bursts:

```python
# In run_server()
for _ in range(prewarm_count):
    pool.release(CppHttpProtocol(...))
```

## WebSocket Connection Pool

WebSocket ring buffer capsules are also pooled:

```python
class _WsConnectionPool:
    def __init__(self, maxsize=64):
        self._pool = collections.deque(maxlen=maxsize)
    
    def acquire(self):
        try:
            capsule = self._pool.pop()
        except IndexError:
            capsule = _ws_ring_buffer_create()
        _ws_ring_buffer_reset(capsule)
        return capsule
```

## Pool Trimming

A background task trims excess pooled objects every 120 seconds:

```python
async def _pool_trim(self):
    while True:
        await asyncio.sleep(120)
        target = max(64, len(active_connections) // 2)
        while len(pool) > target:
            pool.popleft()
        gc.collect(0)
```

## The GIL Dance

All C++ ↔ Python transitions follow these rules:

1. **C++ holds the GIL** during `PyObject_Call`, `PyIter_Send`, and `Py_DECREF`
2. **C++ releases the GIL** during:
   - `yyjson_parse_raw` (Phase 1 JSON parsing)
   - `platform_socket_write` / `writev`
   - Socket I/O (handled by asyncio)
3. **Python holds the GIL** during `_handle_async` and business logic execution
4. **I/O suspensions release the GIL** automatically via `await`

This means a single Python process can handle thousands of concurrent connections — the GIL is only held for the microseconds it takes to parse, route, and serialize.

## Pre-Warming at Startup

Before accepting connections, `run_server()` performs multiple layers of pre-warming:

1. **Protocol Object Pool** — pre-allocates 1024 (single-process) or 512 per worker
2. **C++ Buffer Pool** — pre-allocates thread-local response buffers
3. **Cached Refs** — pre-imports modules and interns strings in C++
4. **Route Warmup** — exercises parse → route → serialize to warm the instruction cache
5. **GC Freeze** — moves all startup objects to a permanent generation
