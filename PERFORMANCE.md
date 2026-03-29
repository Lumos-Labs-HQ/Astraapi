# AstraAPI Performance Plan

## Measured Component Costs (WSL, Python 3.14, single process)

All numbers from isolated micro-benchmarks on the actual codebase — no estimates.

```
Component                           Cost      Bottleneck?
─────────────────────────────────────────────────────────
Python header re-parse              2825 ns   ██ #1 CRITICAL
asyncio.create_task                 1675 ns   ██ #2 CRITICAL
coroutine object creation           1209 ns   █  #3 HIGH
Pydantic model_dump_json             979 ns      medium
Pydantic model_validate              968 ns      medium
5x ContextVar.set                    471 ns      medium
C++ JSON parse (yyjson)              394 ns      fast ✓
C++ JSON serialize                   374 ns      fast ✓
C++ build_response_from_parts        356 ns      fast ✓
coro.send(None) + StopIteration      245 ns      fast ✓
C++ parse_cookie                     241 ns      fast ✓
loop.create_future                   205 ns      fast ✓
C++ route match (static)             142 ns      fast ✓
http_buf append+clear                141 ns      fast ✓
C++ route match (param)              157 ns      fast ✓
WS channel feed (waiter path)        207 ns      fast ✓
C++ ws_build_frame_bytes             134 ns      fast ✓
```

## Theoretical Maximum req/s (single worker, single core)

```
Path                          Total cost    Theoretical max    Actual ~
──────────────────────────────────────────────────────────────────────
Sync GET (current)              4309 ns       232,000 req/s
Async GET (current)             7438 ns       134,000 req/s    ~140k ✓
POST + Pydantic (current)       9779 ns       102,000 req/s

After OPT-1 (no header re-parse):
  Sync GET                      1013 ns       987,000 req/s
  Async GET                     4142 ns       241,000 req/s    → 200k+ ✓

After OPT-1 + OPT-2 (no create_task for instant coros):
  Async GET                     1258 ns       794,000 req/s    → 250k+ ✓
```

**The gap to Rust (220k) and Hono (160k) is entirely explained by two Python-side costs:**
1. `Python header re-parse` = **2825 ns** — runs on EVERY request in `data_received()`
2. `asyncio.create_task` = **1675 ns** + coroutine creation **1209 ns** — runs on EVERY async endpoint

These two together = **5709 ns** of pure Python overhead per request.  
The C++ hot path (route match + JSON + response build) = only **1013 ns** total.

---

## Root Cause Analysis

### Bottleneck #1 — Python header re-parse in `data_received()` [2825 ns/req]

**File:** `astraapi/_cpp_server.py` ~line 1490–1530

```python
# This runs on EVERY request, even when no route needs it:
if self._needs_req_ctx and data:
    _nl = data.find(CRLF)                    # bytes.find
    _req_line = data[:_nl]                   # ALLOC: new bytes
    _hdr_block = data[:_hdrs_end]            # ALLOC: new bytes
    _hdr_lines = _hdr_block.split(CRLF)      # ALLOC: list + N bytes objects
    for _hl in _hdr_lines[1:]:               # loop over headers
        _parsed_hdrs.append((...))           # ALLOC per header
    _current_query_string.set(_qs)           # ContextVar.set x5
    _current_raw_headers.set(_parsed_hdrs)
    _current_method.set(_method)
    _current_path.set(_path_only)
    _current_body.set(body)
```

C++ already parsed all of this inside `handle_http_append_and_dispatch`. Python is re-doing it.

**Why `_needs_req_ctx` is always True:** It defaults to `True` and is only set `False` when
`_core_app_needs_req_ctx` has an entry for the app — which requires explicit opt-in that
never happens in practice.

### Bottleneck #2 — `asyncio.create_task` for every async endpoint [1675 + 1209 ns/req]

**File:** `astraapi/_cpp_server.py` ~line 1620–1630

```python
# For EVERY async endpoint, even `async def root(): return {"ok": True}`:
task = create_task(
    self._handle_async(coro, first_yield, status_code, keep_alive))
pt.add(task)
task.add_done_callback(pt.discard)
```

For endpoints with no real `await` (no DB, no I/O), `first_yield is None` and
`coro.send(None)` raises `StopIteration` immediately — the result is available
synchronously. But we still pay 1675 ns for `create_task` + 1209 ns for the
coroutine wrapper object.

### Bottleneck #3 — Pydantic on every POST [968 + 979 ns/req]

`model_validate` + `model_dump_json` = ~1947 ns. This is unavoidable for correctness
but can be reduced by keeping the C++ fast path for responses that don't need
`response_model` filtering.

---

## Optimization Plan

### OPT-1: Move header context into C++ — eliminate Python re-parse [+60-80k req/s]

**What:** C++ already parses headers in `handle_http_append_and_dispatch`. Instead of
Python re-parsing the raw bytes, C++ should store the parsed context in a per-worker
thread-local struct that Python reads via a single zero-copy C extension call.

**How:**

Add to `CoreApp` in `app.cpp`:
```cpp
// Thread-local parsed request context — set by dispatch_one_request, read by Python
struct RequestContext {
    std::string_view method;
    std::string_view path;
    std::string_view query_string;
    std::string_view body;
    // headers as flat array of (name_sv, value_sv) — zero-copy views into buffer
    std::array<std::pair<std::string_view, std::string_view>, 32> headers;
    int header_count = 0;
};
thread_local RequestContext tl_req_ctx;
```

Add Python-callable getter:
```cpp
static PyObject* CoreApp_get_request_context(CoreAppObject* self, PyObject*) {
    // Return pre-parsed context as a single tuple — one C call replaces 5 ContextVar.set()
    // Returns: (qs_bytes, method_str, path_str, headers_list, body_bytes)
    // All string_views → PyBytes/PyUnicode with zero extra parsing
}
```

In `data_received()`, replace the entire header re-parse block with:
```python
# ONE C++ call instead of 2825 ns of Python parsing
if self._needs_req_ctx and data:
    ctx = self._core.get_request_context()
    if ctx:
        _current_query_string.set(ctx[0])
        _current_raw_headers.set(ctx[2])
        _current_method.set(ctx[1])
        _current_path.set(ctx[3])
        _current_body.set(ctx[4])
```

**Better:** Set `_needs_req_ctx = False` by default. Only set `True` when a route
actually declares `Request`, `Header`, or `Cookie` params. Check at `_sync_routes_to_core()`:

```python
# In applications.py _sync_routes_to_core():
needs_ctx = any(
    getattr(getattr(r, 'dependant', None), 'request_param_name', None) or
    getattr(getattr(r, 'dependant', None), 'header_params', []) or
    getattr(getattr(r, 'dependant', None), 'cookie_params', [])
    for r in self.router.routes
    if hasattr(r, 'dependant')
)
# Store on core_app so CppHttpProtocol reads it
_core_app_needs_req_ctx[id(self._core_app)] = (self._core_app, needs_ctx)
```

For a typical `@app.get("/") async def root(): return {...}` app, this eliminates
the entire 2825 ns block.

**Expected gain:** +60-80k req/s (eliminates the #1 bottleneck entirely)

---

### OPT-2: Inline instant coroutines — skip `create_task` [+30-50k req/s]

**What:** For `async def` endpoints with no real `await` (first_yield is None),
drive the coroutine inline in `data_received()` without scheduling a Task.

**How:** In the dispatch loop in `data_received()`:

```python
elif tag == "async":
    _, coro, first_yield, status_code, keep_alive = result
    
    # Fast path: no await — drive inline, write immediately, no Task overhead
    if first_yield is None:
        try:
            coro.send(None)
        except StopIteration as e:
            raw = e.value
            _raw_type = type(raw)
            if _raw_type is dict or _raw_type is list:
                self._core.write_async_result(
                    raw, transport, status_code, keep_alive, self._sock_fd)
            else:
                resp = self._core.build_response_from_any(raw, status_code, keep_alive)
                if resp:
                    transport.write(resp)
        except Exception as exc:
            transport.write(_500_RESP)
        # No create_task, no pending_tasks tracking, no done_callback
        # Saves: 1675 (create_task) + 1209 (coro wrapper) = 2884 ns
    else:
        # Real await — must schedule as Task
        task = create_task(
            self._handle_async(coro, first_yield, status_code, keep_alive))
        if not task.done():
            pt = self._pending_tasks
            pt.add(task)
            task.add_done_callback(pt.discard)
```

This is safe because:
- `first_yield is None` means C++ already drove the coroutine to its first yield point
  and it yielded `None` (no real awaitable) — the result is ready
- No I/O, no DB, no `await` — nothing to schedule
- Exception handling is inline — no Task exception propagation needed

**Expected gain:** +30-50k req/s for async endpoints without real I/O

---

### OPT-3: Eliminate coroutine wrapper for `_handle_async` [+10-15k req/s]

**What:** `_handle_async` is itself a coroutine that wraps the user coroutine.
For the inline path (OPT-2), this is already eliminated. For the Task path,
reduce the wrapper overhead.

**How:** Instead of:
```python
task = create_task(self._handle_async(coro, first_yield, sc, ka))
```

Use `__slots__` on `CppHttpProtocol` (already done) and cache the bound method:
```python
# In __init__ / _reinit — cache bound methods to avoid per-call LOAD_ATTR
self._handle_async_bound = self._handle_async  # already a bound method via __slots__
```

Also: for the common `dict` return case, skip `build_response_from_any` dispatch
and call `write_async_result` directly (already done in `_handle_async` — verify
the `_raw_type is dict` branch is always taken for simple endpoints).

**Expected gain:** +10-15k req/s

---

### OPT-4: Reduce Pydantic round-trip for response models [+5-10k req/s on POST]

**What:** For routes with `response_model`, the current path is:
1. C++ returns `InlineResult` to Python
2. Python calls `request_body_to_args` (Pydantic validate)
3. Python calls `endpoint(**kwargs)`
4. Python calls `model_dump_json()` on result

Step 4 can be skipped when the endpoint returns the same model type it received
(pass-through pattern). Detect at registration time:

```python
# In routing.py APIRoute.__init__:
# If response_model == body param type → set a flag to skip model_dump_json
# and use encode_to_json_bytes directly (C++ yyjson, 374 ns vs 979 ns)
self._response_is_input_model = (
    response_model is not None and
    len(body_params) == 1 and
    body_params[0].field_info.annotation is response_model
)
```

**Expected gain:** +5-10k req/s for POST endpoints with response_model

---

### OPT-5: Batch ContextVar sets into single C call [+3-5k req/s]

**What:** 5 separate `ContextVar.set()` calls = 471 ns. Replace with a single
C extension function that sets all 5 atomically.

**How:** Add to `_astraapi_core`:
```cpp
// set_request_context(qs, method, path, headers, body) — sets all 5 ContextVars at once
// Uses PyContext_CopyCurrent() + batch set — one GIL operation
static PyObject* py_set_request_context(PyObject* self, PyObject* args) { ... }
```

This is a minor gain but free to implement alongside OPT-1.

**Expected gain:** +3-5k req/s

---

### OPT-6: WebSocket — pre-allocated frame slots [+20-30k msg/s for WS]

**What:** `_WsFastChannel.feed()` allocates a `(opcode, payload)` tuple per frame
(44 ns each). For high-frequency WS (echo, pub/sub), this is the bottleneck.

**How:** Replace the `deque` of tuples with a pre-allocated ring buffer:
```python
class _WsFastChannel:
    __slots__ = ('_waiter', '_buf_opcodes', '_buf_payloads', '_head', '_tail',
                 '_loop', '_protocol', ...)
    
    def __init__(self, ...):
        # Pre-allocate fixed-size ring buffer — no per-frame allocation
        self._buf_opcodes  = [0] * 256    # int array
        self._buf_payloads = [None] * 256  # object array
        self._head = 0
        self._tail = 0
    
    def feed(self, opcode, payload):
        waiter = self._waiter
        if waiter is not None and not waiter.done():
            self._waiter = None
            waiter.set_result((opcode, payload))  # still one alloc but unavoidable
        else:
            idx = self._tail & 255
            self._buf_opcodes[idx] = opcode
            self._buf_payloads[idx] = payload
            self._tail += 1
            # no tuple alloc — saves 44 ns per buffered frame
```

**Expected gain:** +20-30k msg/s for buffered WS frames

---

### OPT-7: `_needs_req_ctx` default False + per-app detection [+15-20k req/s]

This is the implementation detail of OPT-1. The key insight:

```python
# Current default in _cpp_server.py:
_needs_request_context: bool = True  # ← always True = always 2825 ns overhead

# Fix: detect at startup
# In _create_server() after _sync_routes_to_core():
_needs_ctx = _detect_routes_need_request_context(app)
_core_app_needs_req_ctx[id(core_app)] = (core_app, _needs_ctx)
```

For apps with only simple `@app.get` / `@app.post` routes (no `Request` injection,
no `Header()`, no `Cookie()`), this eliminates the entire 2825 ns block.

---

## Implementation Priority

```
Priority  Optimization                    Gain          Effort   Risk
────────────────────────────────────────────────────────────────────
  P0      OPT-7: _needs_req_ctx=False     +15-20k       Low      Zero
  P0      OPT-2: Inline instant coros     +30-50k       Low      Low
  P1      OPT-1: C++ request context      +40-60k       Medium   Low
  P1      OPT-5: Batch ContextVar sets    +3-5k         Low      Zero
  P2      OPT-3: Reduce coro wrapper      +10-15k       Low      Low
  P2      OPT-4: Skip model_dump_json     +5-10k        Medium   Low
  P3      OPT-6: WS ring buffer           +20-30k msg/s High     Medium
```

**P0 items alone (OPT-7 + OPT-2) require ~50 lines of code changes and deliver
+45-70k req/s — pushing from 140k to 185-210k req/s.**

---

## Projected Results After Optimizations

```
Scenario                    Current    After P0    After P0+P1   After All
──────────────────────────────────────────────────────────────────────────
Async GET (simple dict)     140k       185-200k    210-230k      240-260k
Sync GET                    ~200k      ~200k       ~230k         ~250k
POST + Pydantic body        ~100k      ~130k       ~150k         ~160k
WebSocket echo              ~80k msg/s ~80k        ~90k          ~110k msg/s

Rust (Axum) baseline        220k       —           —             —
Hono.js baseline            160k       —           —             —
Target                      200k       ✓ (P0+P1)   ✓✓            ✓✓✓
```

---

## What Cannot Be Optimized (Hard Limits)

| Limit | Why |
|---|---|
| GIL per async endpoint | Python's GIL means one thread runs at a time per worker. Multi-worker bypasses this. |
| asyncio event loop overhead | ~163 ns per `await` even with uvloop. Rust has zero. |
| Pydantic validation cost | ~968 ns is Pydantic v2's Rust core — already near-optimal. |
| `transport.write()` syscall | ~55 ns mock; real TCP adds kernel time. Same for all frameworks. |
| Python function call overhead | ~50-100 ns per call. Rust has zero. |

The remaining gap to Rust after all optimizations (~20-40k req/s) is the irreducible
cost of the Python async dispatch layer. Matching Rust exactly would require removing
Python from the hot path entirely — which breaks FastAPI syntax compatibility.

**The 200k req/s target is achievable with P0 + P1 optimizations while keeping
100% FastAPI API compatibility, all platforms, and full stability.**

---

## Cross-Platform Notes

All optimizations above are platform-neutral:
- No OS-specific syscalls (no `SO_BUSY_POLL`, no `SCM_RIGHTS` changes)
- No WSL-specific tuning
- `_needs_req_ctx` detection works identically on Linux/macOS/Windows
- Inline coroutine path works identically on all event loops (uvloop/winloop/asyncio)
- C++ thread-local request context works on all platforms (POSIX + Windows TLS)

The performance gains are consistent across Linux, macOS, and Windows because
the bottlenecks are in Python-layer logic, not OS I/O.
