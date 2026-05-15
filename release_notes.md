# AstraAPI 0.2.1 Release Notes

**PyPI:** https://pypi.org/project/astraapi/0.2.1  
**Python:** 3.14+ | **C++:** 20

---

## Overview

AstraAPI 0.2.1 is a stability and compatibility release for our FastAPI-compatible web framework powered by a compiled C++20 core. This release fixes editor IntelliSense, resolves Windows build failures, hardens CI/CD pipelines, and raises the minimum Python version to **3.14**.

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def hello():
    return {"message": "Hello World"}

if __name__ == "__main__":
    app.run(port=8000, workers=4)
```

---

## Breaking Changes

### Minimum Python Version Raised to 3.14
- **Dropped support for Python 3.12 and 3.13.**
- The codebase leverages `annotationlib` (PEP 649 deferred annotations) and other 3.14-specific runtime features that are not back-portable.
- Wheels are now built **exclusively** for `cp314` (Linux x86_64/aarch64, macOS arm64, Windows x64).
- Updated all docs, Docker examples, and CI matrices to reflect `python:3.14-slim`.

---

## What's New

### Editor IntelliSense & Static Analysis
- **Added `astraapi/py.typed`** — PEP 561 marker so Pylance, Pyright, and mypy recognize the package as fully typed.
- **Added `astraapi/__init__.pyi`** — Stub file with explicit re-exports of all 20 public symbols (`AstraAPI`, `Depends`, `HTTPException`, `Request`, `Response`, `APIRouter`, `WebSocket`, etc.).
- **Root cause:** `astraapi/__init__.py` used `__getattr__` lazy loading for fast cold-start (~1 s vs ~4.7 s). While great for runtime, it completely broke static resolution. The `.pyi` stub restores autocomplete, "Go to Definition", and type checking without changing runtime behavior.

### Windows Build Fixes
- **`cpp_core/src/app.cpp`** — Added `#define NOMINMAX` before `Python.h` to prevent Windows `min`/`max` macros from clashing with `std::min`/`std::max` (MSVC `C2589` / `C2059`).
- **`cpp_core/src/streaming_multipart.cpp`** — Replaced POSIX-only `mkstemp` / `write` / `close` with cross-platform `std::fopen` / `std::fwrite` / `std::fclose` + `std::filesystem::temp_directory_path()`.
- **`cpp_core/include/streaming_multipart.hpp`** — Renamed enum value `ERROR` → `ERR` in both `MultipartState` and `FeedResult`. Windows defines `ERROR` as a macro (`0`), causing `FeedResult::ERROR` to expand to `FeedResult::0` (syntax error).
- **`scripts/build_core.sh`** — Now passes `-DPython3_EXECUTABLE="$(python -c 'import sys; print(sys.executable)')"` to CMake to guarantee the C++ extension is built against the same Python that runs it.

### CI/CD Hardening
- **`.github/workflows/tests.yml`** — Switched from `pip` to `uv` for faster installs. Installs `".[dev,standard]"` to ensure `httpx`, `jinja2`, `python-multipart`, and other standard extras are present during testing. Uses Python 3.14 explicitly (matching local dev and `uv`'s managed Python).
- **`.github/workflows/publish.yml`** — `CIBW_BUILD` narrowed to `cp314-*` only. Fixed CMake Python version mismatch that caused segfaults when the runtime Python differed from the build-time Python.

### Test Stability
- **`astraapi/_testclient.py`** — TestClient shared server registry now uses `_app_instance_id` (monotonic counter) instead of `id(app)`. Python's allocator recycles object addresses, causing servers to be incorrectly reused across tests.
- **`astraapi/applications.py`** — Custom route classes now pass `dep_inject_mask=0x07` so the C++ core injects `__raw_headers__`, `__method__`, `__path__` directly into kwargs, eliminating a ContextVar fallback race in `_asgi_shim`.

### Documentation
- **VitePress `base` path** — Changed from `/Astraapi/` to `/` to support custom root domains (e.g., `https://astraapi.lumoslab.tech/`). Previously CSS/assets 404'd when the site was served at a domain root instead of a GitHub Pages subdirectory.
- **Dev dependencies** — Added missing test-only packages to `[project.optional-dependencies] dev`: `starlette`, `httpx`, `jinja2`, `python-multipart`, `email-validator`, `pydantic-settings`, `pydantic-extra-types`, `websockets`, `uvicorn`.

---

## Architecture

| Layer | Technology |
|-------|-----------|
| HTTP parser | llhttp (Node.js parser, compiled into C++ extension) |
| JSON encoder | yyjson (zero-allocation, SIMD-accelerated) |
| JSON serializer | Custom C++20 string builder with SSE2/NEON small-string optimization |
| Compression | libdeflate (optional, ~3× faster than zlib) |
| WebSocket framing | Custom C++ frame parser + ring buffer |
| Response serialization | C++ `HTTPResponse` → Python `bytes` via memoryview (zero-copy when possible) |
| Dependency injection | C++ topological sort (`compute_dependency_order`) at route registration time |
| Parameter extraction | C++ scalar coercion + batch validation (avoids per-request Python loops) |

---

## Performance

| Metric | Value | Notes |
|--------|-------|-------|
| Cold import | ~1.0 s | `__getattr__` lazy loading defers pydantic/openapi until first use |
| Hello World RPS | 450K+ | Single process, keep-alive, C++ core serving directly |
| Latency p99 | 0.8 ms | `wrk -t4 -c400 -d30s` on AMD Ryzen 9 |
| JSON serialization | ~2× faster | Custom C++ string builder vs `json.dumps` |
| Multi-process | Linear scaling | `app.run(workers=N)` uses `SO_REUSEPORT` + pre-fork |

*Benchmarks: Python 3.14, C++20, AMD Ryzen 9, 16 workers, wrk2*

---

## Installation

```bash
pip install astraapi
```

Requires **Python 3.14+** and a C++20-capable compiler (GCC 10+, Clang 12+, MSVC 2019+). Pre-built wheels are provided for Linux x86_64/aarch64, macOS arm64, and Windows x64. The C++ extension builds automatically from source if a wheel is not available for your platform.

---
