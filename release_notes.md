# AstraAPI 0.2.11 Release Notes

**PyPI:** https://pypi.org/project/astraapi/0.2.11  
**Python:** 3.14+ | **C++:** 20

---

## Overview

Patch release with CORS reliability fixes, Server-Sent Events (SSE) support, C++ build fixes, and deep DI/validation/middleware performance optimizations.

---

## New Features

### Server-Sent Events (SSE)

- **`astraapi/_response.py`** — Added `EventSourceResponse` and `ServerSentEvent` classes for native SSE streaming. `EventSourceResponse` extends `StreamingResponse` with `media_type = "text/event-stream"` and automatically encodes `ServerSentEvent` dataclass instances (including `data`, `event`, `id`, `retry` fields) into SSE-formatted chunks. Re-exported via `astraapi.responses`.

---

## Bug Fixes

### CORS Headers on All Response Paths

- **`astraapi/applications.py`** — When `CORSMiddleware` is registered, `_sync_routes_to_core_body` now forces `_needs_request_context = True`. This ensures the C++ fast path parses raw headers into the `_current_raw_headers` ContextVar so that async Python handlers (`_handle_stream`, `_handle_async`, `_handle_async_di`, `_handle_pydantic`, `_handle_middleware_result`) and `_dispatch_exception` can extract the `Origin` header and inject CORS headers.
- **`astraapi/_cpp_server.py`** — `run_server` also sets `_needs_request_context = True` whenever `CORSMiddleware` is detected in `app.user_middleware`.

### C++ Build Fix

- **`cpp_core/src/app.cpp`** — Added missing `#include "compat.hpp"` to resolve `'LIKELY' was not declared in this scope` and `'UNLIKELY' was not declared in this scope` compiler errors.

### Response Model Validation/Serialization Robustness

- **`astraapi/_compat/v2.py`** — `ModelField.validate()` now catches `TypeError` and `AttributeError` in addition to `ValidationError`. Pydantic v2's `validate_python(..., from_attributes=True)` can raise these exceptions when re-validating model instances that contain nested dicts (e.g., `model_construct` bypass) or when computed fields access missing attributes.
- **`astraapi/applications.py`** — Both `_make_response_model_shim` and `_make_response_class_shim` now wrap `response_field.serialize()` in try-except to catch `TypeError` and `AttributeError` and convert them to `ResponseValidationError` instead of letting raw exceptions propagate and crash the request.

---

## Performance Improvements

### CORS Origin Extraction Deduplication

- **`astraapi/_cpp_server.py`** — Added `_extract_origin(raw_headers)` helper that scans the raw header list once and extracts the `Origin` value. Replaced 6 copy-pasted inline origin-scan blocks across `_dispatch_exception`, `_handle_async`, `_handle_stream`, `_handle_async_di`, `_handle_middleware_result`, and `_handle_pydantic`.

### Middleware Chain Optimizations

- **`astraapi/_cpp_server.py`** — Hoisted `Request`, `Response`, and `JSONResponse` imports in `_run_http_middleware` from per-call to module level. Added `_ConstNext` class (with `__slots__`) to eliminate per-layer closure allocation in the middleware dispatch chain.

### CORSMiddleware Optimizations

- **`astraapi/_middleware_impl.py`** — Added `_CorsSendWrapper` class with `__slots__` to replace the per-request `_send_wrapper` closure inside `CORSMiddleware.__call__`. Pre-computed `frozenset` for `allow_headers` enables O(1) preflight header validation instead of rebuilding a `set` on every preflight request. Switched to raw-bytes comparison before `decode()` when scanning for the `Origin` header.

### GZipMiddleware Optimization

- **`astraapi/_middleware_impl.py`** — Hoisted `import gzip` from inside `GZipMiddleware.__call__` to module level.

### Dependency Injection Import Hoisting

- **`astraapi/routing.py`** — Hoisted `BackgroundTasks`, `SecurityScopes`, `contextmanager`, `TypeAdapter`, and `_cpp_server` ContextVars (`_current_raw_headers`, `_current_method`, `_current_path`, `_current_query_string`) to module level. All three DI resolvers (`_resolve_deps_sync`, `_resolve_deps_async`, `_resolve_deps_gen`) now use these module-level references instead of inline `try/except ImportError` blocks on every call.

---

## Installation

```bash
pip install astraapi
```

Requires **Python 3.14+** and a C++20-capable compiler (GCC 10+, Clang 12+, MSVC 2019+). Pre-built wheels are provided for Linux x86_64/aarch64, macOS arm64, and Windows x64.
