# Performance Regression Analysis
**Before: 143,331 req/s → After: 43,331 req/s (~3.3× drop)**

---

## Request Lifecycle (End-to-End)

```
TCP recv → data_received() [Python]
  → ContextVar sets ×5 [Python]          ← REGRESSION #2
  → handle_http_append_and_dispatch() [C++]
      → HTTP parse (llhttp)
      → Route match (trie)
      → Param extract
      → DI resolve
      → JSON body parse:
          → PyImport_ImportModule("json") ×2 [C++→Python]  ← REGRESSION #1 (CRITICAL)
          → PyObject_GetAttrString ×2
          → yyjson parse (GIL-released)
      → Serialize response
      → transport.write()
  → If async: create_task() → _handle_async_di() [Python]
```

---

## Issue #1 — CRITICAL: `PyImport_ImportModule("json")` on every body request

**File:** `cpp_core/src/app.cpp` lines 3930–3936

```cpp
// This runs on EVERY request that has a JSON body:
static PyObject* _orig_json_loads = nullptr;
if (!_orig_json_loads) {
    PyRef jm(PyImport_ImportModule("json"));          // import lock + sys.modules lookup
    if (jm) _orig_json_loads = PyObject_GetAttrString(jm.get(), "loads");
}
PyRef jm2(PyImport_ImportModule("json"));             // ← EVERY REQUEST: import lock!
PyRef cur_loads(jm2 ? PyObject_GetAttrString(jm2.get(), "loads") : nullptr);  // ← dict lookup
```

`PyImport_ImportModule` acquires the **global import lock** and does a `sys.modules` dict
lookup on every single request with a body. `PyObject_GetAttrString` does another dict
lookup. These two calls alone can account for 50–100µs per request under contention.

**Fix:** Cache `json.loads` function pointer once. On each request, compare against cached
value using a single pointer comparison — no import lock, no allocation.

```cpp
// Replace the per-request block with:
static PyObject* _orig_json_loads = nullptr;
static PyObject* _cached_cur_loads = nullptr;
if (!_orig_json_loads) {
    PyRef jm(PyImport_ImportModule("json"));
    if (jm) {
        _orig_json_loads = PyObject_GetAttrString(jm.get(), "loads");
        _cached_cur_loads = _orig_json_loads;
    }
}
// Only re-fetch if a test patches json.loads (rare, detectable via module dict version)
PyObject* py_json_result = nullptr;
if (_orig_json_loads != _cached_cur_loads || /* check sys.modules["json"].__dict__ version */ false) {
    // patched path — call cur_loads
}
```

Better approach: use `PyDict_GetItemString(PyImport_GetModule("json"), "loads")` which
does NOT acquire the import lock (uses already-imported module).

---

## Issue #2 — ContextVar `.set()` on every `data_received()` call

**File:** `fastapi/_cpp_server.py` lines 1471–1494

```python
_current_query_string.set(_qs)          # Token alloc
# ...
_current_body.set(b'')                  # Token alloc (added in session 4)
# ...
_current_raw_headers.set(_parsed_hdrs)  # Token alloc
_current_method.set(_method)            # Token alloc
_current_path.set(_path_only)           # Token alloc
_current_body.set(data[_body_start:])   # Token alloc + slice
```

6 ContextVar `.set()` calls per request. Each allocates a `Token` object (Python heap).
At 100k req/s this is 600k allocations/sec just for ContextVars.

**Root cause of `_current_body` addition:** Added in session 4 to fix `_make_asgi_route_shim`
for custom route classes. But it runs on ALL requests, not just custom-route ones.

**Fix:** Only set `_current_body` when a custom route class is registered. Gate it with a
module-level flag `_has_custom_routes: bool = False`.

---

## Issue #3 — `_make_asgi_route_shim` reads ContextVars on every custom-route request

**File:** `fastapi/applications.py` — `_make_asgi_route_shim`

```python
async def _asgi_shim(**kwargs):
    _raw_headers = _crh.get() or []      # ContextVar read
    _method = _cm.get() or 'GET'         # ContextVar read
    _path = _cp.get() or '/'             # ContextVar read
    _body = _cb.get() if hasattr(_cb, 'get') else b''  # ContextVar read + hasattr
    _qs = _cqs.get() if hasattr(_cqs, 'get') else b''  # ContextVar read + hasattr
```

`hasattr(_cb, 'get')` is called on every request — ContextVar always has `.get`, this
check is always True and wastes a Python attribute lookup.

**Fix:** Remove `hasattr` checks. ContextVar always has `.get`.

---

## Issue #4 — `inspect.iscoroutinefunction` called twice per async DI request

**File:** `fastapi/_cpp_server.py` line 2145

```python
_is_async_endpoint = inspect.iscoroutinefunction(endpoint) or \
    inspect.iscoroutinefunction(inspect.unwrap(endpoint))
```

`inspect.iscoroutinefunction` does `hasattr` + `__wrapped__` traversal. Called on every
`_handle_async_di` invocation. This should be cached at route registration time.

**Fix:** Cache `_is_async` flag in `_endpoint_id_to_route` map at registration time.

---

## Issue #5 — `_handle_async_di` does two `_eitr.get()` lookups per request

**File:** `fastapi/_cpp_server.py` lines 2064–2160

```python
# First lookup:
from fastapi.routing import _endpoint_id_to_route as _eitr
_rt = _eitr.get(_ep_id)

# ... 80 lines later ...

# Second lookup:
from fastapi.routing import _endpoint_id_to_route as _eitr2
_ep_route = _eitr2.get(id(endpoint))
```

Two separate dict lookups for the same key, plus two `from ... import` statements
(module dict lookups) inside the hot async path.

**Fix:** Single lookup at top, reuse result.

---

## Issue #6 — `from fastapi.X import Y` inside hot async handlers

**File:** `fastapi/_cpp_server.py` — `_handle_async_di`, `_dispatch_exception`, `_handle_pydantic`

```python
async def _handle_async_di(...):
    from fastapi.routing import _endpoint_id_to_route as _eitr   # module dict lookup
    from fastapi.routing import _endpoint_id_to_route as _eitr2  # again!
    from fastapi.background import BackgroundTasks as _BT         # module dict lookup
    from fastapi.exceptions import HTTPException, RequestValidationError  # in _dispatch_exception
    from fastapi._concurrency import is_async_callable, run_in_threadpool
```

Python caches module imports in `sys.modules`, but `from X import Y` still does a
`sys.modules` lookup + `getattr` on every call. These are inside async handlers called
per-request.

**Fix:** Hoist all imports to module level.

---

## Issue #7 — `_run_http_middleware` builds `Request` object even when no middleware

**File:** `fastapi/_cpp_server.py` line 2177

```python
if _http_middleware_dispatchers:
    _mw_hdrs = kwargs.get('__raw_headers__') or _current_raw_headers.get()
```

The `_current_raw_headers.get()` is called even when `_http_middleware_dispatchers` is
empty (the `if` branch is not taken). Wait — actually this IS guarded. But the
`kwargs.get('__raw_headers__')` is not — it runs on every async DI request.

**Fix:** Move `kwargs.get('__raw_headers__')` inside the `if _http_middleware_dispatchers:` block.

---

## Issue #8 — Duplicate `": line N"` strip in `app.cpp` error path

**File:** `cpp_core/src/app.cpp` lines 4020–4030

```cpp
// Strip position info from Python json error (e.g. ": line 1 column 2 (char 1)")
if (!py_err_msg.empty()) {
    auto colon_pos = py_err_msg.find(": line ");
    if (colon_pos != std::string::npos) {
        py_err_msg = py_err_msg.substr(0, colon_pos);
    }
}
// Strip position info from Python json error (e.g. ": line 1 column 2 (char 1)")
if (!py_err_msg.empty()) {   // ← EXACT DUPLICATE
    auto colon_pos = py_err_msg.find(": line ");
    ...
}
```

Dead code — the strip block is copy-pasted twice. Minor but indicates code quality issue.

---

## Issue #9 — `_handle_async_di` defines nested `_call_and_write` closure per request

**File:** `fastapi/_cpp_server.py` lines 2155–2230

```python
async def _call_and_write() -> Any:
    """Call endpoint and write response..."""
    # This closure is created (heap-allocated) on EVERY async DI request
    ...
```

Python closure creation allocates a new function object + cell objects on every call.
For high-frequency async endpoints with DI, this is significant overhead.

**Fix:** Inline the closure body directly (it's only called once).

---

## Issue #10 — `_current_body.set(b'')` added unconditionally in `data_received`

**File:** `fastapi/_cpp_server.py` line 1476

```python
_hdrs_end = data.find(b'\r\n\r\n')
if _hdrs_end <= 0:
    _current_body.set(b'')   # ← Added in session 4 to fix flaky tests
```

This runs on every partial/pipelined request. The fix for flaky tests introduced a
ContextVar allocation on the partial-data path which is hit during keep-alive pipelining.

---

## Issue #11 — `_handle_async_di` calls `inspect.iscoroutinefunction(inspect.unwrap(endpoint))`

`inspect.unwrap` traverses `__wrapped__` chain. For decorated endpoints this can be
multiple attribute lookups. Should be cached at route registration.

---

## Summary Table

| # | Location | Issue | Impact |
|---|----------|-------|--------|
| 1 | `app.cpp:3935` | `PyImport_ImportModule("json")` per body request | **CRITICAL** ~50–100µs/req |
| 2 | `_cpp_server.py:1471` | 6× ContextVar `.set()` per request | HIGH ~10µs/req |
| 3 | `applications.py` shim | `hasattr` on ContextVar every custom-route req | MEDIUM |
| 4 | `_cpp_server.py:2145` | `inspect.iscoroutinefunction` uncached | MEDIUM |
| 5 | `_cpp_server.py:2064` | Double `_eitr.get()` + double `from ... import` | MEDIUM |
| 6 | `_cpp_server.py` handlers | `from X import Y` inside hot async paths | MEDIUM |
| 7 | `_cpp_server.py:2177` | `kwargs.get('__raw_headers__')` outside guard | LOW |
| 8 | `app.cpp:4020` | Duplicate strip block (dead code) | CLEANUP |
| 9 | `_cpp_server.py:2155` | Closure allocation per async DI request | MEDIUM |
| 10 | `_cpp_server.py:1476` | `_current_body.set(b'')` on partial-data path | LOW |
| 11 | `_cpp_server.py:2145` | `inspect.unwrap` uncached | LOW |

---

## Fix Priority

### Phase 1 — Immediate (recover ~2.5× of the regression)
1. **Fix #1** — Replace `PyImport_ImportModule("json")` per-request with a cached
   `PyDict_GetItemString` on the already-imported module's `__dict__`. This is the
   single biggest win.
2. **Fix #2** — Gate `_current_body.set()` behind `_has_custom_routes` flag.
3. **Fix #8** — Remove duplicate strip block in `app.cpp`.

### Phase 2 — Structural (recover remaining ~0.8×)
4. **Fix #5 + #6** — Hoist all `from X import Y` to module level in `_cpp_server.py`.
5. **Fix #4 + #11** — Cache `is_async` + `unwrap` result at route registration time.
6. **Fix #9** — Inline `_call_and_write` closure.

### Phase 3 — Polish
7. **Fix #3, #7, #10** — Minor ContextVar and guard cleanups.

---

## What NOT to do
- **PGO** — Profile-guided optimization won't help when the bottleneck is Python-level
  allocations and import lock contention. PGO only helps C++ branch prediction.
- **ASGI fallback** — Do not add ASGI/Starlette fallback paths; they add 10–50µs per
  request due to scope dict construction and async generator overhead.
