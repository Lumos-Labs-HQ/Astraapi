# Installation

AstraAPI supports Python 3.14+ and provides pre-built wheels for Linux, macOS, and Windows. The C++ core compiles automatically during installation if a wheel is not available.

## Requirements

- Python 3.14 or newer
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

## What You Get Out of the Box

AstraAPI's `pyproject.toml` already includes these performance-critical packages:

| Package | Purpose | Platform |
|---------|---------|----------|
| `uvloop` | Faster asyncio event loop | Linux/macOS |
| `winloop` | Faster asyncio event loop | Windows |
| `orjson` | Fast JSON serialization | All |
| `watchfiles` | Hot reload file watcher | All |

You do **not** need to install these separately. They are already dependencies.

## Optional Extras

For additional features, install with extras:

```bash
# Standard extras: test client, templates, forms, email validation, settings
pip install "astraapi[standard]"

# All extras: everything above + ujson, pyyaml, itsdangerous
pip install "astraapi[all]"
```

| Extra | Packages | Purpose |
|-------|----------|---------|
| `httpx` | TestClient HTTP requests | Testing |
| `jinja2` | Template rendering | HTML templates |
| `python-multipart` | Form and file upload parsing | Forms/files |
| `email-validator` | Email field validation | Validation |
| `pydantic-settings` | Settings management | Configuration |
| `pydantic-extra-types` | Extra Pydantic types | Validation |
| `ujson` | UJSONResponse | Alternative JSON |

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

### Optional System Libraries

For maximum performance, install these before building:

```bash
# Ubuntu/Debian
sudo apt-get install libdeflate-dev libbrotli-dev

# macOS
brew install libdeflate brotli
```

If libdeflate is available, AstraAPI uses it for 2-3x faster gzip compression. Otherwise, it falls back to zlib.

## Verify Installation

```python
import astraapi
print(astraapi.__version__)
```

## Troubleshooting

### ImportError: cannot import name 'AstraAPI'

Make sure you installed `astraapi` (with two `a`s), not `astrapi` or similar.

### Build fails with "CMake not found"

Install CMake:
- Ubuntu/Debian: `sudo apt-get install cmake`
- macOS: `brew install cmake`
- Windows: `choco install cmake` or download from cmake.org

### libdeflate not found warning

If you see this at startup:
```
[astraapi] libdeflate not found; using zlib (2-3x slower compression).
```

Install libdeflate and rebuild:
```bash
sudo apt-get install libdeflate-dev
pip install --no-binary :all: astraapi
```

## Next Steps

- [Quick Start](./quickstart) — write your first app
