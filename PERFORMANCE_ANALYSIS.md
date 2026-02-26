
#### 3.1 `PyUnicode_FromStringAndSize` per-request allocations in `handle_http`

**Location**: `cpp_core/src/app.cpp` lines 2700-2960

**Problem**: Every request creates fresh `PyUnicode` objects for header values, query params, cookie values, path params, method strings, and path strings. For a simple `GET /` with 10 headers, that's ~20+ `PyUnicode_FromStringAndSize` calls = 20 heap allocations + 20 `Py_DECREF`s.

**Fix**: Intern common header values and method strings. Cache frequently-seen path strings. The code already interns dictionary keys (`py_field_name` in `FieldSpec`) but not the **values**.

```cpp
// Example: method strings are one of 7 values — cache them
static PyObject* g_method_GET = nullptr;
static PyObject* g_method_POST = nullptr;
// ... etc, initialized in py_init_cached_refs()

// In handle_http, replace:
//   PyRef method_str(PyUnicode_FromStringAndSize(req.method.data, req.method.len));
// With:
//   PyObject* method_str = get_cached_method(req.method.data, req.method.len);
```

**Estimated improvement**: ~10-15% for simple endpoints (1-3 fewer allocations per header param).

---

#### 3.2 Redundant `PyDict_New()` for kwargs even when route has no params

**Location**: `cpp_core/src/app.cpp` lines 2665-2670

**Problem**: The code correctly checks `needs_kwargs` but the logic still creates a `PyDict_New()` for almost every request. For zero-param endpoints like `GET /health`, the dict is created and never used.

**Current code**:
```cpp
bool needs_kwargs = (match->param_count > 0) || spec.has_query_params || ...;
PyRef kwargs(needs_kwargs ? PyDict_New() : nullptr);
```

**Fix**: For truly zero-param sync endpoints, skip `kwargs` entirely and call `endpoint()` with `PyObject_CallNoArgs()` instead of `PyObject_Call(endpoint, args, kwargs)`.

**Estimated improvement**: ~5% for zero-param endpoints.

---

#### 3.3 JSON body parsing holds GIL during `yyjson_doc_to_pyobject`

**Location**: `cpp_core/src/app.cpp` lines 2895-2910

**Problem**: The raw yyjson parse releases the GIL (`Py_BEGIN_ALLOW_THREADS`), but the conversion from `yyjson_doc` to Python objects (`yyjson_doc_to_pyobject` → `val_to_pyobject`) runs **with the GIL held**, because it calls `PyDict_New`, `PyUnicode_FromString`, etc. For large JSON bodies (10KB+), this can be significant.

**Fix**: For the common case of a JSON body that maps directly to kwargs, instead of `parse → convert to Python dict → extract fields from dict`, parse directly to the kwargs dict:

```cpp
// Instead of: parse JSON → PyDict → loop over spec → extract
// Do: parse JSON → for each yyjson key, check spec map → add to kwargs directly
```

This avoids creating an intermediate Python dict for the entire body.

**Estimated improvement**: ~15-20% for POST endpoints with JSON bodies.

---

### IMPORTANT: P1 — Medium Impact Improvements

#### 3.4 `_handle_async` creates excessive Python objects

**Location**: `fastapi/_cpp_server.py` lines 1380-1460

**Problem**: `_handle_async()` checks `type(raw)` against 7 types via sequential `is` comparisons. After that, `self._core.build_response(raw, status_code, keep_alive)` re-enters C++ which re-serializes. The type-checking should happen in C++ to avoid the Python-to-C++ round trip.

**Fix**: Have C++ `_drive_coro` return the raw result directly to a C++ response builder that handles the type dispatch internally.

**Estimated improvement**: ~5-8% for async endpoints.

---

#### 3.5 Query string parsing missing percent-decoding

**Location**: `cpp_core/src/app.cpp` lines 2700-2720

**Problem**: Query string parsing iterates character-by-character creating `string_view` for each key/value pair, then does a hash map lookup. Percent-decoding is completely missing from the C++ query parser. The codebase has `percent_decode.hpp` but it's not used in query string parsing.

**Fix**:
1. Add percent-decoding inline (the codebase has `percent_decode.hpp` but it's unused here).
2. For multi-value query params (`?tag=a&tag=b`), the current code overwrites — only the last value survives. This is a correctness issue.

**Estimated improvement**: Correctness fix + ~2% from avoiding missed params.

---

#### 3.6 `_WsFastChannel.feed()` type-check overhead

**Location**: `fastapi/_cpp_server.py` lines 105-125

**Problem**: Every WebSocket frame goes through `type(payload)` check and `len(payload)` in Python. This is already handled in C++ — the Python layer re-does it.

**Fix**: Have C++ `ws_handle_and_feed` return the byte count so Python doesn't need to recompute `len()`.

**Estimated improvement**: ~3-5% for high-throughput WebSocket.

---

#### 3.7 Rate limiter `std::string` key allocation

**Location**: `cpp_core/src/app.cpp` lines 2616-2660

**Problem**: `shard.counters[self->current_client_ip]` does a `std::string` copy for the map key lookup every time. Since `current_client_ip` is set once per connection, this is a hash + compare per request.

**Fix**: Cache the shard iterator for the current connection:

```cpp
// At connection time:
auto it = shard.counters.try_emplace(self->current_client_ip, RateLimitEntry{}).first;
// At request time:
auto& entry = it->second;  // O(1), no hash
```

**Estimated improvement**: ~2% when rate limiting is enabled.

---

### NICE-TO-HAVE: P2 — Low Impact Improvements

#### 3.8 GC disabled with no memory safety net

**Location**: `fastapi/_cpp_server.py` lines 1778-1783

**Problem**: `gc.disable()` is called, and `gc.collect(1)` is only run every 600 seconds during idle. If the server is never idle and has cyclic references (e.g., from user code with closures, lambda captures), memory will grow unbounded.

**Fix**: Add a memory-based threshold trigger:

```python
import resource  # or psutil

def _gc_maintenance():
    if active_count[0] == 0:
        gc.collect(1)
    else:
        # Safety: if RSS grew >100MB since last check, do gen0
        current_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        if current_rss > _last_rss + 100_000:  # ~100MB
            gc.collect(0)  # fast gen0 only
    loop.call_later(600.0, _gc_maintenance)
```

---

#### 3.9 Missing `Py_TPFLAGS_DISALLOW_INSTANTIATION` on internal types

**Location**: `cpp_core/src/app.cpp` — `InlineResultType`, `MatchResultType`, etc.

These types use `Py_TPFLAGS_DEFAULT` but should use `Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION` to prevent accidental construction from Python and allow CPython to skip certain checks.