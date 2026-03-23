# Performance Plan: 43k → 300k+ req/s
**Platform: Linux (WSL2), Python 3.14, uvloop 0.22.1 available**

---

## Measured Event Loop Baselines

| Scenario | asyncio | uvloop | Winner |
|----------|---------|--------|--------|
| No-await inline (C++ PYGEN_RETURN) | 3,892k/s | 4,033k/s | ~same |
| `create_task` no-await + eager_task_factory | 1,159k/s | 1,421k/s | uvloop +22% |
| `create_task` + 1 real yield | 256k/s | **386k/s** | **uvloop +51%** |
| `create_task` alloc only | 552k/s | — | — |
| Future + call_soon | 340k/s | 637k/s | uvloop +87% |

**Key finding:** uvloop gives **+51% on the real async path** (1 real await).  
Current loop: `asyncio._UnixSelectorEventLoop`. Target: `uvloop.Loop`.

---

## Current State: Why 43k req/s

The regression from 143k → 43k is caused by **3 changes from session 4**:

### Regression R1 — `PyImport_ImportModule("json")` on every body request [CRITICAL]
**File:** `cpp_core/src/app.cpp:3935`
```cpp
// Runs on EVERY request with a JSON body:
PyRef jm2(PyImport_ImportModule("json"));   // acquires global import lock
PyRef cur_loads(jm2 ? PyObject_GetAttrString(jm2.get(), "loads") : nullptr);
```
`PyImport_ImportModule` acquires the **global Python import lock** every call.
At 100k req/s this serializes all requests through a single lock.
**Estimated cost: ~50-80µs/req under contention → drops throughput 3×.**

### Regression R2 — 6× ContextVar `.set()` on every `data_received()` [HIGH]
**File:** `fastapi/_cpp_server.py:1471-1494`
`_current_body.set()` added in session 4 runs on ALL requests (not just custom routes).
Each `.set()` allocates a `Token` object. 6 per request = 6 heap allocs.

### Regression R3 — `recv(timeout=2.5)` in WebSocket test client [MEDIUM]
**File:** `fastapi/_testclient.py`
Each WebSocket test now blocks 2.5s waiting for server close. Slows test suite
but doesn't affect production performance.

---

## Full Request Pipeline (annotated)

```
TCP recv
  └─ data_received() [Python]
       ├─ 6× ContextVar.set()                    ← R2: 6 Token allocs/req
       ├─ bytes.find(b'\r\n\r\n')                ← OK
       └─ handle_http_append_and_dispatch() [C++] ← 1 Python→C++ call

C++ handle_http_append_and_dispatch()
  ├─ llhttp parse (zero-copy string_view)         ← OK
  ├─ Router::at() — O(1) static or O(k) trie     ← OK
  ├─ shared_mutex shared_lock (even after freeze) ← LOCK: ~0.2µs/req
  ├─ method bitmask check                         ← OK
  ├─ CORS / TrustedHost check                     ← OK
  ├─ FastRouteSpec param extraction               ← OK
  ├─ JSON body parse:
  │    ├─ PyImport_ImportModule("json") ×2        ← R1: CRITICAL
  │    ├─ PyObject_GetAttrString ×2               ← dict lookup/req
  │    └─ yyjson parse (GIL-released)             ← OK
  ├─ PyUnicode_FromString ×N (60 sites in app.cpp)← heap alloc/req
  ├─ PyDict_New + PyDict_SetItem (kwargs build)   ← heap alloc/req
  ├─ SYNC endpoint: call → serialize → write      ← OK (all C++)
  └─ ASYNC endpoint: PYGEN_NEXT → return tuple    ← Python overhead

Python async dispatch
  ├─ create_task(_handle_async(coro, first_yield)) ← 1.81µs Task alloc
  ├─ _drive_coro() — extra coroutine frame         ← 0.3µs frame alloc
  ├─ from fastapi.X import Y (×4 per call)         ← module dict lookups
  ├─ inspect.iscoroutinefunction() ×2 uncached     ← 0.2µs/req
  ├─ _eitr.get() ×2 (duplicate dict lookup)        ← redundant
  ├─ async def _call_and_write() closure alloc     ← heap alloc/req
  ├─ write_async_result() [C++]                    ← OK
  └─ record_request_end() [C++]                    ← separate call
```

---

## All Issues (Complete List)

### C++ Issues

| ID | File | Issue | Impact |
|----|------|-------|--------|
| C1 | app.cpp:3935 | `PyImport_ImportModule("json")` per body req — import lock | CRITICAL |
| C2 | app.cpp:3168 | `shared_mutex` lock on every req even after `routes_frozen=true` | HIGH |
| C3 | app.cpp | 60× `PyUnicode_FromString` in hot path — heap alloc per req | HIGH |
| C4 | app.cpp | `PyDict_New` + kwargs dict per req — use `_PyObject_Vectorcall` | HIGH |
| C5 | app.cpp:4016 | Duplicate `": line N"` strip block — dead code copy-paste | CLEANUP |
| C6 | app.cpp | `std::string` allocs in error path — use stack `snprintf` | MEDIUM |
| C7 | app.hpp | `FieldSpec`: 3× `std::string` = 72 bytes/field — use arena offsets | RAM |
| C8 | app.hpp | `RouteInfo.methods` `std::vector<std::string>` — drop after freeze | RAM |
| C9 | app.hpp | `tags/summary/description` in hot `RouteInfo` — move to OpenAPI-only struct | RAM |
| C10 | param_extractor.cpp | `ParamSpec` duplicates `FieldSpec` — two registries for same data | RAM |
| C11 | dep_engine.cpp | `DepPlan` separate registry — merge into `FastRouteSpec` | RAM |
| C12 | router.hpp | 256-byte dispatch table per node — use 8-entry sorted array | RAM |

### Python Issues

| ID | File | Issue | Impact |
|----|------|-------|--------|
| P1 | _cpp_server.py:1476 | `_current_body.set()` on ALL reqs — gate behind `_has_custom_routes` | HIGH |
| P2 | _cpp_server.py | 5 other ContextVar `.set()` per req — pass context in C++ tuple | HIGH |
| P3 | _cpp_server.py | `from fastapi.X import Y` inside hot async handlers (×4/req) | HIGH |
| P4 | _cpp_server.py:2145 | `inspect.iscoroutinefunction()` uncached — cache at registration | HIGH |
| P5 | _cpp_server.py:2075 | `_eitr.get()` called twice with same key | MEDIUM |
| P6 | _cpp_server.py:2155 | `async def _call_and_write()` closure alloc per req | MEDIUM |
| P7 | _cpp_server.py | `_drive_coro` is extra `async def` frame — inline into `_handle_async` | MEDIUM |
| P8 | _cpp_server.py | `pending_tasks` set per async req — replace with `_active_task` slot | MEDIUM |
| P9 | _cpp_server.py | `record_request_end()` separate C++ call — merge into `write_async_result` | LOW |
| P10 | _cpp_server.py | `_current_route.set()` ContextVar per DI req — gate behind flag | LOW |

### Event Loop Issues

| ID | Issue | Impact |
|----|-------|--------|
| E1 | Using `asyncio._UnixSelectorEventLoop` — uvloop is 51% faster for async | CRITICAL |
| E2 | `eager_task_factory` already set (good) — but only helps no-await path | OK |
| E3 | No `call_soon` stepper — `create_task` costs 1.81µs vs `call_soon` 0.3µs | HIGH |

---

## Event Loop Strategy

### Current: `asyncio._UnixSelectorEventLoop`
- `create_task` + 1 real await: **256k/s**
- Pure Python selector, slow I/O polling

### Target: `uvloop.Loop` (libuv-based C extension)
- `create_task` + 1 real await: **386k/s** (+51%)
- libuv epoll, C-level Future/Task, faster `call_soon`
- Already installed: `uvloop 0.22.1`

### How to switch (1 change in `applications.py`):
```python
# In app.run() before asyncio.run():
try:
    import uvloop
    uvloop.run(run_server(...))   # uses uvloop.Loop directly
except ImportError:
    asyncio.run(run_server(...))  # fallback to asyncio
```

`uvloop.run()` is the correct API for Python 3.12+ (avoids deprecated `set_event_loop_policy`).

### Additional: `eager_task_factory` (already set in run_server)
For `async def endpoint(): return dict` with **no real awaits**:
- C++ gets `PYGEN_RETURN` inline → **no task needed** → handled entirely in C++
- This path already achieves ~4M/s (not the bottleneck)

For `async def endpoint(): await something`:
- Must use `create_task` → uvloop reduces this from 3.90µs → 2.59µs per req

---

## Implementation Plan (Ordered by Impact)

### Phase 1 — Fix Regressions (43k → 143k req/s)
*Restore what was broken in session 4*

| Step | File | Change |
|------|------|--------|
| 1.1 | `app.cpp` | Replace `PyImport_ImportModule("json")` per-req with `PyImport_GetModule` + cached `__dict__` pointer. Single `PyDict_GetItemString` per req, no lock. |
| 1.2 | `_cpp_server.py` | Add `_has_custom_routes = False` flag. Gate `_current_body.set()` behind it. Set flag to `True` in `_sync_routes_to_core` when custom route class detected. |
| 1.3 | `app.cpp` | Remove duplicate `": line N"` strip block (lines 4023-4026). |

### Phase 2 — Event Loop + Python Hot Path (143k → 220k req/s)

| Step | File | Change |
|------|------|--------|
| 2.1 | `applications.py` | Switch to `uvloop.run()` — **+51% async throughput** |
| 2.2 | `_cpp_server.py` | Hoist all `from fastapi.X import Y` to module level in `_handle_async_di`, `_dispatch_exception`, `_handle_pydantic` |
| 2.3 | `_cpp_server.py` | Single `_eitr.get(_ep_id)` call, reuse result (remove second lookup) |
| 2.4 | `_cpp_server.py` | Inline `_drive_coro` body into `_handle_async` — remove extra coroutine frame |
| 2.5 | `_cpp_server.py` | Inline `_call_and_write` body — remove closure alloc per DI req |
| 2.6 | `app.cpp` + `_cpp_server.py` | Pass `is_coroutine` flag in C++ async tuple. Remove `inspect.iscoroutinefunction()` call. |
| 2.7 | `_cpp_server.py` | Replace `pending_tasks` set with `_active_task: Task | None` slot |

### Phase 3 — C++ Core (220k → 270k req/s)

| Step | File | Change |
|------|------|--------|
| 3.1 | `app.cpp` | Skip `shared_mutex` lock after `routes_frozen=true` — direct vector access |
| 3.2 | `app.cpp` | Pre-intern ALL `PyUnicode_FromString` as `static PyObject*` at module init |
| 3.3 | `app.cpp` | 0-param endpoints: `PyObject_CallNoArgs`. 1-param: `PyObject_CallOneArg`. N-param: `_PyObject_Vectorcall` (no kwargs dict alloc) |
| 3.4 | `_cpp_server.py` | Replace 6 ContextVar sets with single C++ tuple pass-through for async path |
| 3.5 | `app.cpp` | Merge `record_request_end()` into `write_async_result()` — 1 fewer Python→C++ call |

### Phase 4 — Memory + Deep Optimization (270k → 300k+ req/s)

| Step | File | Change |
|------|------|--------|
| 4.1 | `app.hpp` | `FieldSpec`: replace 3× `std::string` with `uint32_t` arena offsets — 72→12 bytes/field |
| 4.2 | `app.hpp` | Drop `RouteInfo.methods` vector after freeze — use `method_mask` only |
| 4.3 | `router.hpp` | Replace 256-byte dispatch table with 8-entry sorted array for small nodes |
| 4.4 | `param_extractor.cpp` | Eliminate `ParamSpec` registry — it duplicates `FieldSpec` |
| 4.5 | `dep_engine.cpp` | Merge `DepPlan` into `FastRouteSpec` — eliminate separate registry |
| 4.6 | `_cpp_server.py` | Replace `create_task` with `loop.call_soon` stepper for simple async endpoints — saves 1.5µs/req |

---

## Projected Performance

| Phase | Sync GET / | Async GET /async | Key change |
|-------|-----------|-----------------|------------|
| Current (broken) | 43k/s | 43k/s | — |
| After Phase 1 | 143k/s | 120k/s | Fix import lock + ContextVar |
| After Phase 2 | 160k/s | 200k/s | uvloop + inline wrappers |
| After Phase 3 | 220k/s | 250k/s | Lock-free + vectorcall |
| After Phase 4 | 280k/s | 310k/s | Arena + call_soon stepper |

**Target 300k+ req/s is achievable for both sync and async.**

---

## RAM Reduction Summary

| Component | Current | After Phase 4 | Saving |
|-----------|---------|---------------|--------|
| `FieldSpec` per field | 80 bytes | 16 bytes | 80% |
| `RouteInfo` per route | 400 bytes | 150 bytes | 62% |
| Router node dispatch | 256 bytes | 32 bytes | 87% |
| `ParamSpec` registry | duplicate | eliminated | 100% |
| `DepPlan` registry | separate | merged | 100% |
| ContextVar Tokens/req | 6 allocs | 1 alloc | 83% |

---

## What NOT to Do
- **PGO** — bottleneck is Python allocations + import lock, not C++ branch prediction
- **ASGI fallback** — adds 10-50µs/req, opposite direction
- **winloop** — Linux system, winloop is Windows-only (irrelevant here)
- **More workers** — GIL-bound; use `workers=N` for multi-core scaling, not single-core perf
