# Installation

AstraAPI supports Python 3.10+ and provides pre-built wheels for Linux, macOS, and Windows. The C++ core compiles automatically during installation if a wheel is not available.

## Requirements

- Python 3.10, 3.11, 3.12, 3.13, or 3.14
- A C++ compiler (GCC 11+, Clang 14+, or MSVC 2022+) if building from source
- CMake 3.20+

## Using pip

```bash
pip install astraapi
```

## Using uv (recommended)

```bash
uv add astraapi
```

## Optional: uvloop

While AstraAPI works with the standard `asyncio` event loop, **uvloop is strongly recommended** for production:

```bash
pip install uvloop
```

AstraAPI automatically detects and uses uvloop when available — no code changes needed.

## Build from Source

If you want the bleeding edge or are on an unsupported platform:

```bash
git clone https://github.com/your-org/astraapi.git
cd astraapi
pip install -e ".[dev]"
```

The build uses `scikit-build-core` and CMake. It automatically:

1. Compiles the C++ HTTP core (with llhttp, yyjson, ryu)
2. Links optional system libraries (libdeflate, brotli, zlib)
3. Copies the shared object to the package directory

## Verify Installation

```python
import astraapi
print(astraapi.__version__)
```

## What You Do NOT Need

Unlike FastAPI, AstraAPI does **not** require:

| Package | Why Not Needed |
|---------|---------------|
| `uvicorn` | AstraAPI has a built-in server |
| `gunicorn` | AstraAPI has built-in multi-worker support |
| `hypercorn` | Not needed |
| `daphne` | Not needed |

You can still use these if you have existing infrastructure, but they are not required.

## Optional Dependencies

| Package | Purpose |
|---------|---------|
| `uvloop` | Faster asyncio event loop (strongly recommended) |
| `ujson` | Faster JSON for `UJSONResponse` |
| `orjson` | Faster JSON for `ORJSONResponse` |
| `httpx` | Async HTTP client for testing |
| `pytest-asyncio` | For running async test suites |

## Troubleshooting

### ImportError: cannot import name 'AstraAPI'

Make sure you installed `astraapi` (with two `a`s), not `astrapi` or similar.

### Build fails with "CMake not found"

Install CMake:
- Ubuntu/Debian: `sudo apt-get install cmake`
- macOS: `brew install cmake`
- Windows: `choco install cmake` or download from cmake.org

### Segfault on shutdown

This was a known issue in early versions where cleanup hooks were registered twice. Upgrade to the latest version:

```bash
pip install -U astraapi
```

## Next Steps

- [**Quick Start**](./quickstart) — write your first app
