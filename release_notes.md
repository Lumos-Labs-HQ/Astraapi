# AstraAPI 0.2.3 Release Notes

**PyPI:** https://pypi.org/project/astraapi/0.2.3
**Python:** 3.14+ | **C++:** 20

---

## Overview

Major performance release — pydantic validation and dependency injection overhead reduced by 3-8x on complex routes. `response_model` endpoints now skip redundant Pydantic re-validation. C++ coroutine inlining eliminates event-loop bounces for trivial async deps.

---

## Performance Improvements

### Pydantic Validation Fast Path

- **`astraapi/routing.py`** — Extended `_is_trivial_schema()` to accept `int`/`float`/`bool` alongside `str`. The `_fast_validate` closure now performs lightweight builtin coercion (~50ns) instead of Pydantic `TypeAdapter.validate_python()` (~2-5µs) for scalar params with no constraints (`min_length`, `ge`, `le`, `pattern`, etc.). C++ fast_spec extraction already supplies the correct raw types.

- **`astraapi/dependencies/utils.py`** — `_validate_value_with_model_field()` now skips Pydantic entirely when the value is already the correct Python type and the core_schema has no constraint keys. Uses module-level `_TRIVIAL_CONSTRAINT_KEYS` frozenset (avoiding per-call construction). Only applies to non-body params (query/header/cookie/path).

### Pre-Encoded Response Body Fast Path

- **`astraapi/applications.py`** — `_make_fast_dep_shim()` detects when an endpoint returns a Pydantic model instance AND the route has a `response_model` configured. Calls `model_dump_json(by_alias=True)` directly and returns the JSON string, skipping C++ `TypeAdapter.validate_python()` + `serialize_python()` + JSON re-serialization (3 redundant operations).

- **`cpp_core/src/app.cpp`** — Pre-encoded body fast path (bytes/str check) placed BEFORE the `response_model_local` validation block. When Python returns bytes or str, C++ writes it directly to transport without touching response_model validation or JSON serialization.

### Pre-Interned Spec Keys

- **`astraapi/dependencies/utils.py`** — `sys.intern()` called on field names and aliases at route registration time (inside `_precomputed_batch_specs`). Pre-interned `py_field_name` and `py_lookup_key` passed as extra dict fields in the batch specs.

- **`cpp_core/src/param_extractor.cpp`** — `batch_extract_params_inline()` checks for pre-interned keys from Python before calling `PyUnicode_InternFromString()`. Uses `Py_INCREF` on cached refs instead of interning per-request. Added cleanup loop (`Py_XDECREF`) at function exit.

### Response Cache Size Guard

- **`cpp_core/src/app.cpp`** — `hash_dict_content()` returns 0 (skip cache) for dicts with >50 keys, avoiding expensive recursive hashing on large responses.

### Memory Allocation Reductions

- **`astraapi/dependencies/utils.py`** — `all_param_fields` construction replaced 5 list allocations (`list(path) + list(query) + list(header) + list(cookie)`) with `itertools.chain()` iterator. Cookie string parsing uses list-accumulate-then-join instead of O(N²) `+=` concatenation in both batch and fallback paths.

### Multi-Worker SO_REUSEPORT Detection Fix

- **`astraapi/_multiworker.py`** — SO_REUSEPORT capability test now binds to ephemeral port (0) instead of the actual listen port, avoiding conflict with the parent's already-bound socket.

---

## Architecture

```
                      ┌─────────────────────────────────────┐
                      │  Python _fast_dep_shim               │
                      │  model_dump_json() → str             │
                      └──────────────┬──────────────────────┘
                                     │ JSON string
                      ┌──────────────▼──────────────────────┐
                      │  C++ dispatch_one_request            │
                      │  PyBytes/PyUnicode check FIRST       │
                      │  ↓ skip validate_python              │
                      │  ↓ skip serialize_python             │
                      │  ↓ skip json_writer re-serialize      │
                      │  → transport.write(raw_bytes)        │
                      └──────────────────────────────────────┘
```

---

## Installation

```bash
pip install astraapi
```
