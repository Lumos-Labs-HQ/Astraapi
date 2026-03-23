# Master Performance Plan: 43k → 300k+ req/s
**Platform: Linux (WSL2), Python 3.14.3, uvloop 0.22.1, single-core**
**Last updated: 2026-03-23**

---

## 1. Current State Audit

### 1.1 Measured Baselines

| Scenario | asyncio | uvloop | Delta |
|----------|---------|--------|-------|
| No-await inline (C++ PYGEN_RETURN) | 3,892k/s | 4,033k/s | +3.6% |
| `create_task` no-await + eager_task_factory | 1,159k/s | 1,421k/s | +22% |
| `create_task` + 1 real yield | 256k/s | **386k/s** | **+51%** |
| Future + call_soon | 340k/s | 637k/s | +87% |
| Full async req (current server) | ~43k/s | — | — |
| Full sync req (current server) | ~43k/s | — | — |
| Pre-regression baseline | 143k/s | — | — |
| Target | — | 300k+/s | — |

### 1.2 Event Loop — Current vs Target

```
Current:  asyncio._UnixSelectorEventLoop  (Python selector, slow)
Target:   uvloop.Loop                     (libuv C extension, +51% async)

applications.py line 5944-5947:
    asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())  ← DEPRECATED in 3.14
    asyncio.run(run_server(...))                              ← ignores policy!

Fix: replace with uvloop.run(run_server(...))
```

### 1.3 Full Request Pipeline (annotated with costs)

```
TCP recv
  └─ data_received() [Python, called per TCP segment]
       ├─ bytes.find(b'\r\n')                    0.1µs  OK
       ├─ _current_query_string.set()            0.3µs  Token alloc
       ├─ bytes.find(b'\r\n\r\n')               0.1µs  OK
       ├─ bytes.split(b'\r\n') × N headers      0.5µs  list alloc
       ├─ _current_raw_headers.set()             0.3µs  Token alloc
       ├─ _current_method.set()                  0.3µs  Token alloc
       ├─ _current_path.set()                    0.3µs  Token alloc
       ├─ _current_body.set() × 2               0.6µs  2 Token allocs (BUG: always runs)
       └─ handle_http_append_and_dispatch() [C++]

C++ handle_http_append_and_dispatch()
  ├─ llhttp parse (zero-copy string_view)        0.2µs  OK
  ├─ shared_mutex shared_lock (ALWAYS taken)     0.3µs  BUG: taken even after freeze
  ├─ Router::at() — O(1) static / O(k) trie     0.1µs  OK
  ├─ method bitmask check                        0.0µs  OK
  ├─ CORS check (if enabled)                     0.1µs  OK
  ├─ FastRouteSpec param extraction              0.2µs  OK
  ├─ JSON body parse (if POST):
  │    ├─ PyImport_ImportModule("json") ×2       2.0µs  CRITICAL: import lock every req
  │    ├─ PyObject_GetAttrString("loads") ×2     0.3µs  dict lookup
  │    └─ yyjson parse (GIL-released)            0.3µs  OK
  ├─ 30+ function-local static PyObject*         0.1µs  branch per string (lazy init)
  ├─ PyDict_New + PyDict_SetItem × N             0.5µs  heap alloc per req
  ├─ SYNC: PyObject_Call(endpoint, (), kwargs)   0.3µs  OK (uses PyObject_CallNoArgs for 0-param)
  └─ ASYNC: PyIter_Send → PYGEN_NEXT/RETURN

Python async dispatch (only for async endpoints)
  ├─ create_task(_handle_async(coro, first))     1.8µs  Task alloc (uvloop: 1.3µs)
  ├─ _drive_coro() — extra async def frame       0.3µs  coroutine frame alloc
  ├─ from fastapi.routing import _eitr ×2        0.2µs  module dict lookup ×2/req
  ├─ from fastapi.background import BT           0.1µs  module dict lookup/req
  ├─ from fastapi.exceptions import ...          0.1µs  module dict lookup/req
  ├─ inspect.iscoroutinefunction() ×2 uncached   0.2µs  per req
  ├─ _eitr.get(ep_id) ×2 (same key!)            0.1µs  duplicate lookup
  ├─ async def _call_and_write() closure alloc   0.3µs  new function obj per DI req
  ├─ write_async_result() [C++]                  0.2µs  separate C++ call
  └─ record_request_end() [C++]                  0.1µs  separate C++ call (mergeable)
```

---

## 2. All Issues — Complete Catalogue

### 2.1 Regressions (43k → 143k: must fix first)

| ID | File | Line | Issue | Cost/req |
|----|------|------|-------|----------|
| R1 | app.cpp | 3930–3937 | `PyImport_ImportModule("json")` on EVERY body request — acquires global import lock | ~2µs |
| R2 | _cpp_server.py | 1476,1494 | `_current_body.set()` runs on ALL requests, not just custom-route ones | 0.6µs |
| R3 | app.cpp | 4022–4028 | Duplicate `": line N"` strip block — exact copy-paste, dead second copy | cleanup |

**R1 detail:** `_orig_json_loads` is a function-local static (initialized once, OK), but `jm2 = PyImport_ImportModule("json")` runs on EVERY request with a body. `PyImport_ImportModule` acquires the global import lock even for already-imported modules. At 100k req/s this serializes all requests. Fix: use `PyImport_GetModule` (no lock, returns borrowed ref from sys.modules).

**R2 detail:** `_current_body.set()` was added in session 4 to support custom route classes that need the raw body. But it runs on ALL requests. Gate it behind `_has_custom_routes` flag.

### 2.2 Event Loop Issues

| ID | File | Issue | Impact |
|----|------|-------|--------|
| E1 | applications.py:5944 | `asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())` is deprecated in Python 3.14 AND doesn't work with `asyncio.run()` — uvloop is NOT being used | CRITICAL |
| E2 | applications.py:5947 | `asyncio.run()` ignores the policy set above — must use `uvloop.run()` directly | CRITICAL |
| E3 | _cpp_server.py:2798 | `eager_task_factory` already set — good, keeps no-await path fast | OK |
| E4 | _cpp_server.py | `pending_tasks` is a `set` — add/discard costs O(1) amortized but allocates `Token` per task | MEDIUM |

### 2.3 Python Hot Path Issues

| ID | File | Lines | Issue | Cost/req |
|----|------|-------|-------|----------|
| P1 | _cpp_server.py | 2078 | `from fastapi.routing import _endpoint_id_to_route as _eitr` inside `_handle_async_di` — module dict lookup per call | 0.1µs |
| P2 | _cpp_server.py | 2127 | `from fastapi.routing import _endpoint_id_to_route as _eitr2` — DUPLICATE of P1, same module, same name | 0.1µs |
| P3 | _cpp_server.py | 2131 | `from fastapi.background import BackgroundTasks as _BT` inside hot path | 0.1µs |
| P4 | _cpp_server.py | 1901–1902 | `from fastapi.exceptions import ...` inside `_dispatch_exception` — called on every error | 0.1µs |
| P5 | _cpp_server.py | 2133 | `inspect.iscoroutinefunction(endpoint)` uncached — called per DI request | 0.2µs |
| P6 | _cpp_server.py | 2133 | `inspect.iscoroutinefunction(inspect.unwrap(endpoint))` — double unwrap per req | 0.1µs |
| P7 | _cpp_server.py | 2127–2129 | `_eitr2.get(id(endpoint))` — second lookup with same key as `_eitr.get(_ep_id)` | 0.1µs |
| P8 | _cpp_server.py | 2155 | `async def _call_and_write()` — new closure object allocated per DI request | 0.3µs |
| P9 | _cpp_server.py | 1072 | `_drive_coro` is a separate `async def` — extra coroutine frame per async req | 0.3µs |
| P10 | _cpp_server.py | 1313 | `pending_tasks: set` — set.add + set.discard per async req | 0.1µs |
| P11 | _cpp_server.py | 1460–1494 | Full header parse in Python (split, decode, loop) on every request | 0.8µs |
| P12 | _cpp_server.py | 2081 | `_current_route.set(_rt)` ContextVar per DI req — rarely needed | 0.3µs |

### 2.4 C++ Hot Path Issues

| ID | File | Lines | Issue | Cost/req |
|----|------|-------|-------|----------|
| C1 | app.cpp | 3930 | `PyImport_ImportModule("json")` per body req — import lock (= R1) | 2µs |
| C2 | app.cpp | 3168–3179 | `shared_mutex` `shared_lock` constructed even when `routes_frozen=true` — lock object overhead | 0.3µs |
| C3 | app.cpp | 3890–3896 | 8 function-local `static PyObject*` in JSON error path — lazy init branch per call | 0.1µs |
| C4 | app.cpp | 4304,4311 | `s_deps_ran_key`, `s_bg_key` function-local statics — lazy init in hot DI path | 0.1µs |
| C5 | app.cpp | 4512,4636 | `s_mv_rve_cls`, `s_rve_cls2` function-local statics — lazy init in validation path | 0.1µs |
| C6 | app.cpp | 4902,4934 | `s_serialize`, `s_mw_tag` function-local statics — lazy init in response path | 0.1µs |
| C7 | app.cpp | 5158–5204 | `s_mdj`, `s_by_alias_kw`, `s_asdict`, `s_is_dc` function-local statics — lazy init | 0.1µs |
| C8 | app.cpp | 4773 | `PyObject_Call(endpoint, g_empty_tuple, kwargs)` — passes empty tuple unnecessarily; use `_PyObject_Vectorcall` | 0.2µs |
| C9 | app.cpp | 3114 | `PyUnicode_InternFromString("ws")` inside WebSocket upgrade path — should be global | 0.0µs |
| C10 | app.cpp | 1716,1725 | `PyUnicode_FromString(route.methods[i])` in `get_route_info` — called at startup only, OK | startup |
| C11 | app.cpp | 3786 | `PyUnicode_FromString(ct_str)` in form upload path — content-type string per file | rare |
| C12 | app.cpp | 4421 | `PyUnicode_InternFromString("missing")` in validation error path — should be global | rare |

### 2.5 Memory Layout Issues

| ID | Struct | Current | Problem | Fix |
|----|--------|---------|---------|-----|
| M1 | `FieldSpec` | 3× `std::string` (field_name, alias, header_lookup_key) | 72 bytes of heap-allocated strings per field | Replace with `uint32_t` offsets into a per-route string arena |
| M2 | `RouteInfo` | `std::vector<std::string> methods` | Heap alloc per route, iterated on every request before freeze | Drop after freeze — use `method_mask` only |
| M3 | `RouteInfo` | `std::vector<std::string> tags`, `optional<string> summary/description/operation_id` | OpenAPI-only data in hot struct — wastes cache lines | Move to separate `RouteOpenAPIInfo` struct, pointer from `RouteInfo` |
| M4 | `Router::Node` | `std::array<int16_t, 128> dispatch` — 256 bytes per node | Most nodes have 1–3 children; 256-byte table wastes L1 cache | Replace with sorted `std::array<std::pair<uint8_t,int16_t>, 8>` for nodes with ≤8 children |
| M5 | `param_extractor.cpp` | `ParamSpec` struct + `g_registry` map | Exact duplicate of `FieldSpec` + `FastRouteSpec` — two registries for same data | Eliminate `g_registry` entirely; `param_extractor` functions should use `FastRouteSpec` directly |
| M6 | `dep_engine.cpp` | `DepPlan` + `g_dep_plans` map | Separate registry for dependency topology — never used in hot path | Merge into `FastRouteSpec.dep_plan` or eliminate if unused |
| M7 | `CoreAppObject` | `HotCounters` not cache-line aligned | `total_requests`, `active_requests`, `total_errors` share cache line with other fields | Use `alignas(64)` on a separate allocation (not in-struct due to Python allocator) |
| M8 | `MatchResultObject` | `std::vector<std::pair<string,string>> path_params` | Heap alloc per match even for static routes | Use inline array (already done in `MatchParams`); `MatchResultObject` should mirror this |

### 2.6 Algorithm Issues

| ID | Component | Current Algorithm | Better Algorithm | Gain |
|----|-----------|------------------|-----------------|------|
| A1 | Router static lookup | `unordered_map<string, int>` with `SVHash` | Already O(1) with transparent lookup — OK | — |
| A2 | Router trie dispatch | 128-entry `int16_t` array per node | 8-entry sorted array + linear scan for ≤8 children | 8× less memory, better cache |
| A3 | Header normalization | Per-char lookup table (already done) | SIMD 16-byte chunks (SSE2) | 4× faster for long headers |
| A4 | JSON patch detection | `PyImport_ImportModule` + `GetAttrString` per req | `PyImport_GetModule` + `PyDict_GetItemString` on cached `__dict__` | Eliminates import lock |
| A5 | ContextVar batch | 6 separate `.set()` calls | Single C++ tuple with all context data, one Python unpack | 5 fewer Token allocs |
| A6 | Async task dispatch | `create_task` (1.8µs) | `loop.call_soon` stepper for simple endpoints (0.3µs) | 6× faster for no-IO async |
| A7 | Endpoint call | `PyObject_Call(ep, (), kwargs)` | `_PyObject_Vectorcall(ep, stack, n, NULL)` | Avoids empty tuple + kwargs dict |
| A8 | String interning | 30+ function-local statics with lazy `if (!s_x)` branch | All promoted to file-scope, initialized in `py_init_cached_refs` | Eliminates 30+ branches/req |
| A9 | `is_coroutine` check | `inspect.iscoroutinefunction()` per DI req | Cache flag in `RouteInfo.is_coroutine` (already stored!), pass in C++ tuple | Eliminates Python call |
| A10 | Pointer compression | Raw `PyObject*` (8 bytes each) in hot structs | Not applicable — Python GC requires full pointers | N/A |

**Note on pointer compression (A10):** True pointer compression (like JVM compressed oops) requires a controlled heap base address. CPython's allocator uses the system heap — pointers can be anywhere in 64-bit space. We cannot compress `PyObject*` without a custom allocator. The correct approach is to reduce the NUMBER of pointers (M1–M8), not compress them.

### 2.7 Dead / Unused Code

| ID | File | Lines | Description |
|----|------|-------|-------------|
| D1 | app.cpp | 4022–4028 | Duplicate `": line N"` strip block — identical to lines 4015–4021 |
| D2 | param_extractor.cpp | all 338 lines | `g_registry` + `RouteParamRegistry` — never called from `app.cpp` hot path (verified: 0 grep hits) |
| D3 | dep_engine.cpp | all 258 lines | `g_dep_plans` + `DepPlan` — `compile_dep_plan`/`get_dep_plan` not called in hot path |
| D4 | _cpp_server.py | 2127 | `from fastapi.routing import _endpoint_id_to_route as _eitr2` — exact duplicate of line 2078 |
| D5 | _cpp_server.py | 1901–1902 | `from fastapi.exceptions import HTTPException, RequestValidationError` inside `_dispatch_exception` — these are already imported at module level |
| D6 | applications.py | 5894–5901 | First uvloop/winloop policy block (before `if reload:`) — superseded by second block at 5943 |
| D7 | app.cpp | 3114 | `PyRef ws_tag(PyUnicode_InternFromString("ws"))` — creates new interned string every WebSocket upgrade instead of using a global |


---

## 3. Implementation Plan

### Phase 1 — Fix Regressions (43k → 143k req/s)
*Restore pre-session-4 performance. No new features, pure bug fixes.*

#### Step 1.1 — Fix `PyImport_ImportModule("json")` per request [R1 / C1]
**File:** `cpp_core/src/app.cpp` lines 3930–3937

Current (broken):
```cpp
static PyObject* _orig_json_loads = nullptr;
if (!_orig_json_loads) {
    PyRef jm(PyImport_ImportModule("json"));          // OK: runs once
    if (jm) _orig_json_loads = PyObject_GetAttrString(jm.get(), "loads");
}
PyRef jm2(PyImport_ImportModule("json"));             // BUG: import lock EVERY req
PyRef cur_loads(jm2 ? PyObject_GetAttrString(jm2.get(), "loads") : nullptr);
```

Fix:
```cpp
static PyObject* _orig_json_loads = nullptr;
static PyObject* _json_dict = nullptr;
if (!_orig_json_loads) {
    PyRef jm(PyImport_ImportModule("json"));
    if (jm) {
        _json_dict = PyModule_GetDict(jm.get());      // borrowed, stable for module lifetime
        Py_XINCREF(_json_dict);                       // keep alive
        _orig_json_loads = PyDict_GetItemString(_json_dict, "loads");  // borrowed
        Py_XINCREF(_orig_json_loads);
    }
}
// No lock: PyImport_GetModule checks sys.modules without import lock
PyObject* cur_loads = _json_dict ? PyDict_GetItemString(_json_dict, "loads") : nullptr;
bool json_patched = (cur_loads && _orig_json_loads && cur_loads != _orig_json_loads);
```

**Why this works:** `PyImport_GetModule` (or `PyDict_GetItemString` on `sys.modules`) does NOT acquire the import lock. `PyModule_GetDict` returns a borrowed reference to the module's `__dict__` — stable for the module's lifetime. `PyDict_GetItemString` on the dict is a simple hash lookup, no lock.

#### Step 1.2 — Gate `_current_body.set()` behind `_has_custom_routes` [R2]
**File:** `fastapi/_cpp_server.py`

Add at module level (near line 389):
```python
_has_custom_routes: bool = False  # set True in _sync_routes_to_core if custom APIRoute subclass found
```

In `_sync_routes_to_core` (applications.py ~line 1944), after detecting custom route class:
```python
if _has_custom_route_class:
    import fastapi._cpp_server as _srv
    _srv._has_custom_routes = True
```

In `data_received` (line 1476 and 1494), wrap both `_current_body.set()` calls:
```python
if _has_custom_routes:
    _current_body.set(b'')
# ...
if _has_custom_routes:
    _current_body.set(data[_body_start:] if _body_start < len(data) else b'')
```

#### Step 1.3 — Remove duplicate strip block [R3 / D1]
**File:** `cpp_core/src/app.cpp` lines 4022–4028

Delete the second identical block (lines 4022–4028). The first block (4015–4021) is sufficient.

---

### Phase 2 — Event Loop + Python Hot Path (143k → 220k req/s)

#### Step 2.1 — Switch to `uvloop.run()` [E1 / E2]
**File:** `fastapi/applications.py` lines 5943–5947

Current (broken — policy ignored by asyncio.run):
```python
try:
    import uvloop
    asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())  # deprecated + ignored
except ImportError:
    pass
asyncio.run(run_server(self, host, port, sock=shared_sock, unix_sock=dispatch_sock))
```

Fix:
```python
try:
    import uvloop
    uvloop.run(run_server(self, host, port, sock=shared_sock, unix_sock=dispatch_sock))
except ImportError:
    asyncio.run(run_server(self, host, port, sock=shared_sock, unix_sock=dispatch_sock))
```

Also remove the dead first policy block at lines 5894–5901 [D6].

#### Step 2.2 — Hoist all hot-path imports to module level [P1/P2/P3/P4/D4/D5]
**File:** `fastapi/_cpp_server.py`

Add near top of file (after existing imports):
```python
from fastapi.routing import _endpoint_id_to_route as _endpoint_id_to_route_map
from fastapi.background import BackgroundTasks as _BackgroundTasks
from fastapi.exceptions import HTTPException as _HTTPException, RequestValidationError as _RequestValidationError
from fastapi._concurrency import is_async_callable as _is_async_callable, run_in_threadpool as _run_in_threadpool
```

In `_handle_async_di`: replace all 4 `from X import Y` with the module-level names.
In `_dispatch_exception`: replace the 2 `from X import Y` with module-level names.

#### Step 2.3 — Single `_eitr` lookup, remove duplicate [P7 / D4]
**File:** `fastapi/_cpp_server.py` lines 2078–2129

Replace:
```python
from fastapi.routing import _endpoint_id_to_route as _eitr
_rt = _eitr.get(_ep_id)
# ... 50 lines later ...
from fastapi.routing import _endpoint_id_to_route as _eitr2
_ep_route = _eitr2.get(id(endpoint))
```

With:
```python
_ep_route = _endpoint_id_to_route_map.get(_ep_id)  # single lookup, reuse below
_rt = _ep_route  # same object
```

#### Step 2.4 — Cache `is_coroutine` — eliminate `inspect.iscoroutinefunction` [P5/P6/A9]
**File:** `fastapi/_cpp_server.py` line 2133

The `is_coroutine` flag is already stored in `RouteInfo.is_coroutine` and passed in the C++ async tuple as `PreparedRequestObject.is_coroutine`. Read it directly:

```python
# Replace:
_is_async_endpoint = inspect.iscoroutinefunction(endpoint) or \
    inspect.iscoroutinefunction(inspect.unwrap(endpoint))

# With (is_coroutine already in the PreparedRequest tuple from C++):
_is_async_endpoint = bool(prepared.is_coroutine)  # zero Python calls
```

#### Step 2.5 — Inline `_drive_coro` into `_handle_async` [P9]
**File:** `fastapi/_cpp_server.py`

`_drive_coro` is called as `await _drive_coro(di_coro, first_yield)` — this creates an extra coroutine frame. Inline its body directly into `_handle_async_di` at the call site. The function body is ~40 lines and has no other callers in the hot path.

#### Step 2.6 — Inline `_call_and_write` closure [P8]
**File:** `fastapi/_cpp_server.py` lines 2155–2240

`async def _call_and_write()` is defined and immediately awaited — it's a closure that captures 6 variables. Replace with direct inline code. Saves one function object allocation + one coroutine frame per DI request.

#### Step 2.7 — Replace `pending_tasks` set with single slot [P10]
**File:** `fastapi/_cpp_server.py` lines 1313–1314, 1628–1679

For single-worker single-core operation, at most one async task is in-flight per connection. Replace:
```python
self._pending_tasks: set | None = None
self._pending_tasks_discard = None
```
With:
```python
self._active_task: asyncio.Task | None = None
```

And replace `task.add_done_callback(self._pending_tasks_discard)` with:
```python
self._active_task = task
task.add_done_callback(self._clear_active_task)
```

Where `_clear_active_task` is a method: `def _clear_active_task(self, _): self._active_task = None`

---

### Phase 3 — C++ Core Optimizations (220k → 270k req/s)

#### Step 3.1 — Skip `shared_mutex` lock after freeze [C2]
**File:** `cpp_core/src/app.cpp` lines 3168–3179

Current:
```cpp
bool rt_frozen = self->routes_frozen.load(std::memory_order_acquire);
std::shared_lock lock(self->routes_mutex, std::defer_lock);
if (!rt_frozen) {
    std::unique_lock wlock(self->routes_mutex);
    ...
}
auto match = self->router.at(req.path.data, req.path.len);
```

The `std::shared_lock lock(...)` object is constructed even when `rt_frozen=true` (with `std::defer_lock`). The constructor still initializes the lock object. Fix: don't construct it at all:

```cpp
bool rt_frozen = self->routes_frozen.load(std::memory_order_acquire);
if (!rt_frozen) {
    std::unique_lock wlock(self->routes_mutex);
    if (!self->routes_frozen.load(std::memory_order_relaxed))
        self->routes_frozen.store(true, std::memory_order_release);
}
// No lock object — routes are immutable after freeze
auto match = self->router.at(req.path.data, req.path.len);
```

#### Step 3.2 — Promote all function-local statics to file scope [C3–C9 / A8]
**File:** `cpp_core/src/app.cpp`

Move all 30+ function-local `static PyObject* s_xxx = nullptr` to file scope and initialize them in `py_init_cached_refs()`. This eliminates the `if (!s_xxx)` branch on every call.

Strings to promote (currently function-local):
- `s_ct_type_key`, `s_ct_loc_key`, `s_ct_msg_key`, `s_ct_input_key`, `s_ct_mat_val`, `s_ct_mat_msg`, `s_ct_body_str` (JSON error path)
- `s_body_key`, `s_body_key2`, `s_ct_key`, `s_ct_key2` (body/content-type keys)
- `s_async_di_tag`, `s_deps_ran_key`, `s_bg_key` (DI path)
- `s_mv_rve_cls`, `s_mv_body_kw`, `s_rve_cls2`, `s_rve_body_kw` (validation path)
- `s_serialize`, `s_mw_tag` (response path)
- `s_mdj`, `s_by_alias_kw`, `s_asdict`, `s_is_dc` (model dump path)
- `s_errors_str2`, `s_url_key`, `s_det_key` (error serialization)
- `s_kw_filename`, `s_kw_file`, `s_kw_ct` (file upload)
- `_orig_json_loads`, `_json_dict` (after step 1.1 fix)

#### Step 3.3 — Use `_PyObject_Vectorcall` for endpoint calls [C8 / A7]
**File:** `cpp_core/src/app.cpp` line 4773

Current:
```cpp
coro = PyRef(PyObject_Call(endpoint_local, g_empty_tuple, kwargs.get()));
```

Replace with vectorcall (available since Python 3.9, stable API since 3.12):
```cpp
// Build stack array from kwargs dict
Py_ssize_t nkw = PyDict_GET_SIZE(kwargs.get());
PyObject** stack = (PyObject**)alloca((nkw + 1) * sizeof(PyObject*));
PyObject* kwnames = PyTuple_New(nkw);
Py_ssize_t i = 0;
PyObject *k, *v; Py_ssize_t pos = 0;
while (PyDict_Next(kwargs.get(), &pos, &k, &v)) {
    stack[i] = v;
    PyTuple_SET_ITEM(kwnames, i, Py_NewRef(k));
    i++;
}
coro = PyRef(PyObject_Vectorcall(endpoint_local, stack, 0 | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames));
Py_DECREF(kwnames);
```

**Note:** Only beneficial when kwargs dict is small (≤8 params). For large param counts, `PyObject_Call` with a pre-built dict is comparable. Add a size threshold.

#### Step 3.4 — Merge `record_request_end()` into `write_async_result()` [P12]
**File:** `cpp_core/src/app.cpp` + `fastapi/_cpp_server.py`

Add `record_end: bool = True` parameter to `write_async_result`. When `True`, decrement `active_requests` inside C++ before returning. Eliminates one Python→C++ call per async request.

#### Step 3.5 — Eliminate `param_extractor.cpp` dead registry [D2 / M5]
**File:** `cpp_core/src/param_extractor.cpp`

`g_registry` (RouteParamRegistry) is never accessed from `app.cpp` hot path. The `register_route_params` and `extract_params` functions are exposed as Python-callable module methods but not called from the server loop. Verify with grep, then remove the file from `CMakeLists.txt` SOURCES if confirmed dead.

#### Step 3.6 — Eliminate `dep_engine.cpp` dead registry [D3 / M6]
**File:** `cpp_core/src/dep_engine.cpp`

Same analysis as 3.5. `g_dep_plans` is never read in the hot path. If `compile_dep_plan`/`get_dep_plan` are not called from Python either, remove from build.

---

### Phase 4 — Memory Layout (270k → 300k+ req/s)

#### Step 4.1 — `FieldSpec` string arena [M1]
**File:** `cpp_core/include/app.hpp`, `cpp_core/src/app.cpp`

Current `FieldSpec` has 3× `std::string` = 72 bytes of heap-allocated string data per field (plus 24 bytes of `std::string` metadata = 96 bytes total for strings alone).

Replace with arena offsets:
```cpp
struct FieldSpecArena {
    std::string data;  // all strings concatenated: "field_name\0alias\0header_key\0"
};

struct FieldSpec {
    uint32_t field_name_off;    // offset into arena
    uint32_t field_name_len;
    uint32_t alias_off;
    uint32_t alias_len;
    uint32_t header_key_off;
    uint32_t header_key_len;
    // ... rest unchanged
};
```

`string_view` accessors read directly from arena — zero copy, zero heap alloc per field.

#### Step 4.2 — Drop `RouteInfo.methods` vector after freeze [M2]
**File:** `cpp_core/include/app.hpp`, `cpp_core/src/app.cpp`

After `routes_frozen=true`, `RouteInfo.methods` (vector of strings) is never read — only `method_mask` is used. Add a `freeze()` method to `RouteInfo` that calls `methods.clear(); methods.shrink_to_fit()`. Call it from the freeze path.

#### Step 4.3 — Move OpenAPI fields out of `RouteInfo` [M3]
**File:** `cpp_core/include/app.hpp`

`RouteInfo` currently contains `tags`, `summary`, `description`, `operation_id` — used only for OpenAPI schema generation, never in the hot request path. Move to:
```cpp
struct RouteOpenAPIInfo {
    std::vector<std::string> tags;
    std::optional<std::string> summary;
    std::optional<std::string> description;
    std::optional<std::string> operation_id;
};

struct RouteInfo {
    // ... hot fields only ...
    std::unique_ptr<RouteOpenAPIInfo> openapi;  // null after schema is built
};
```

After `set_openapi_schema()` is called, set `openapi = nullptr` to free the memory.

#### Step 4.4 — Compact `Router::Node` dispatch table [M4 / A2]
**File:** `cpp_core/include/router.hpp`, `cpp_core/src/router.cpp`

Current: `std::array<int16_t, 128> dispatch` = 256 bytes per node.
Most API routers have ≤8 routes, so most nodes have 1–3 children.

Replace with:
```cpp
struct Node {
    std::string prefix;
    // Compact dispatch: sorted by first byte, linear scan for ≤8 entries
    // Falls back to full 128-entry table only when > 8 children
    static constexpr int INLINE_DISPATCH = 8;
    struct DispatchEntry { uint8_t byte; int16_t child_idx; };
    std::array<DispatchEntry, INLINE_DISPATCH> inline_dispatch;
    int inline_dispatch_count = 0;
    std::array<int16_t, 128>* full_dispatch = nullptr;  // heap-allocated only when needed
    // ... rest unchanged
};
```

For ≤8 children: linear scan of `inline_dispatch` (fits in one cache line).
For >8 children: allocate `full_dispatch` table (rare, only for routers with many routes sharing a prefix).

#### Step 4.5 — `MatchResultObject` inline path params [M8]
**File:** `cpp_core/include/app.hpp`

`MatchResultObject.path_params` is `std::vector<std::pair<string,string>>` — heap alloc per match. Replace with inline array mirroring `MatchParams`:
```cpp
typedef struct {
    PyObject_HEAD
    Py_ssize_t route_index;
    // ... flags ...
    struct InlineParam { PyObject* name; PyObject* value; };  // pre-interned at registration
    static constexpr int MAX_INLINE = 8;
    InlineParam path_params[MAX_INLINE];
    int path_param_count;
} MatchResultObject;
```

---

### Phase 5 — Advanced Async Optimization (300k → 350k+ req/s)

#### Step 5.1 — `call_soon` stepper for simple async endpoints [A6]
**File:** `fastapi/_cpp_server.py`

For async endpoints with no real I/O awaits (pure computation), `create_task` costs 1.8µs. A `call_soon`-based stepper costs ~0.3µs.

```python
def _step_coro(coro, loop, write_fn, keep_alive):
    """Drive a coroutine step-by-step via call_soon — avoids Task overhead."""
    try:
        fut = coro.send(None)
        if fut is None:
            # asyncio.sleep(0) — schedule next step
            loop.call_soon(_step_coro, coro, loop, write_fn, keep_alive)
        else:
            # Real future — must use Task for proper await semantics
            loop.create_task(_drive_and_write(coro, fut, write_fn, keep_alive))
    except StopIteration as e:
        write_fn(e.value, keep_alive)
```

Gate behind `route.is_coroutine and not route.has_real_io` flag (set at registration).

#### Step 5.2 — Batch write coalescing [new]
**File:** `fastapi/_cpp_server.py`, `cpp_core/src/app.cpp`

For pipelined HTTP/1.1 requests (multiple requests in one TCP segment), batch all sync responses into a single `transport.write()` call. The C++ batch path already does this (`handle_http_batch_v2`). Verify it's being used and not falling back to per-request writes.

#### Step 5.3 — Zero-copy response for static content [new]
**File:** `cpp_core/src/app.cpp`

For endpoints that return `bytes` directly (e.g. pre-serialized JSON), skip the JSON serialization step entirely. Add a `raw_bytes` fast path in the response pipeline.

---

## 4. Build & Compiler Optimizations

### 4.1 Current Compiler Flags (already good)
```cmake
-O3 -march=native -flto -fno-math-errno -funroll-loops
-fomit-frame-pointer -ftree-vectorize
```

### 4.2 Additional Flags to Add

```cmake
# Add to CMakeLists.txt GCC/Clang section:
-fno-exceptions          # no C++ exceptions in hot path (already using error codes)
-fno-semantic-interposition  # allows inlining across TU boundaries (GCC 9+)
-fipa-pta               # interprocedural pointer analysis
-fprofile-arcs          # enable for PGO step 1 (optional)
```

### 4.3 Link-Time Optimization
LTO (`-flto`) is already enabled. Ensure it's applied to third-party C sources too:
```cmake
set_source_files_properties(${THIRD_PARTY_C_SOURCES} PROPERTIES
    COMPILE_FLAGS "-flto -O3 -march=native"
)
```

### 4.4 SIMD for Header Normalization [A3]
The header normalization loop in `app.cpp` processes one byte at a time. Add SSE2 path:
```cpp
#ifdef __SSE2__
// Process 16 bytes at a time: lowercase + replace '-' with '_'
__m128i chunk = _mm_loadu_si128((__m128i*)(src + i));
// ... SIMD lowercase + replace
#endif
```

---

## 5. Expected Benchmark Results After Each Phase

### Sync GET `/` (no body, no params)

| Phase | Change | Sync req/s | Latency |
|-------|--------|-----------|---------|
| Current (broken) | — | 43,000 | 23.2µs |
| After Phase 1 | Fix import lock + ContextVar | 143,000 | 7.0µs |
| After Phase 2 | uvloop + hoist imports | 165,000 | 6.1µs |
| After Phase 3 | Lock-free + promote statics | 210,000 | 4.8µs |
| After Phase 4 | Arena + compact router | 250,000 | 4.0µs |
| After Phase 5 | Batch write + zero-copy | 280,000 | 3.6µs |

### Async GET `/async` (no body, no params, no real await)

| Phase | Change | Async req/s | Latency |
|-------|--------|------------|---------|
| Current (broken) | — | 43,000 | 23.2µs |
| After Phase 1 | Fix import lock + ContextVar | 120,000 | 8.3µs |
| After Phase 2 | uvloop + inline wrappers | 200,000 | 5.0µs |
| After Phase 3 | Lock-free + promote statics | 240,000 | 4.2µs |
| After Phase 4 | Arena + compact router | 270,000 | 3.7µs |
| After Phase 5 | call_soon stepper | 320,000 | 3.1µs |

### Async POST `/items` (JSON body, Pydantic model)

| Phase | Change | Async POST req/s | Latency |
|-------|--------|-----------------|---------|
| Current (broken) | — | ~20,000 | 50µs |
| After Phase 1 | Fix import lock | 80,000 | 12.5µs |
| After Phase 2 | uvloop + inline wrappers | 120,000 | 8.3µs |
| After Phase 3 | Vectorcall + statics | 150,000 | 6.7µs |
| After Phase 4 | Arena memory | 170,000 | 5.9µs |
| After Phase 5 | Batch write | 190,000 | 5.3µs |

### Summary Table (all endpoints, after all phases)

| Endpoint | Before | After | Gain |
|----------|--------|-------|------|
| GET / (sync) | 43k/s | **280k/s** | 6.5× |
| GET /async (async, no await) | 43k/s | **320k/s** | 7.4× |
| POST /items (JSON + Pydantic) | 20k/s | **190k/s** | 9.5× |
| GET /{id} (path param) | 40k/s | **250k/s** | 6.3× |
| GET /items?q=x (query param) | 38k/s | **240k/s** | 6.3× |

---

## 6. What NOT to Do

| Approach | Reason to Skip |
|----------|---------------|
| PGO (Profile-Guided Optimization) | Bottleneck is Python allocations + import lock, not C++ branch prediction. PGO helps C++ branches, not Python GIL contention. |
| ASGI/Starlette fallback | Adds 10–50µs/req. Opposite direction. |
| winloop | Linux system. winloop is Windows-only. |
| Pointer compression | CPython uses system heap — no controlled base address. Cannot compress `PyObject*`. Reduce pointer COUNT instead (M1–M8). |
| `asyncio.set_event_loop_policy` | Deprecated in Python 3.14. Use `uvloop.run()` directly. |
| More workers (for single-core perf) | GIL-bound. Use `workers=N` for multi-core scaling, not single-core perf. |
| Free-threaded Python (PEP 703) | Experimental in 3.14. GIL removal adds per-object reference count overhead that hurts single-threaded perf. |

---

## 7. Verification

### After each phase, run:
```bash
# Rebuild C++ (if C++ files changed)
cd /mnt/e/temp_git/fastapi-rust/cpp_core
cmake --build build -j$(nproc) && cp build/_fastapi_core.so ../fastapi/_fastapi_core.so

# Start server
cd /mnt/e/temp_git/fastapi-rust
.venv/bin/python test.py &

# Benchmark sync
wrk -c100 -d10s http://localhost:8002/

# Benchmark async
wrk -c100 -d10s http://localhost:8002/async

# Benchmark POST
wrk -c100 -d10s -s post.lua http://localhost:8002/items

# Run tests (must stay at 1937 passing)
.venv/bin/python -m pytest tests/test_tutorial/ -q --tb=no 2>&1 | tail -3
```

### Confirm uvloop is active:
```python
import asyncio
loop = asyncio.get_event_loop()
print(type(loop))  # must be: <class 'uvloop.Loop'>
```
