# Contributing Guide

Thank you for your interest in contributing to AstraAPI!

## Development Setup

### Clone the Repository

```bash
git clone https://github.com/your-org/astraapi.git
cd astraapi
```

### Install Dependencies

```bash
# Using uv (recommended)
uv sync

# Using pip
pip install -e ".[dev]"
```

### Build the C++ Core

```bash
bash scripts/build_core.sh
```

This compiles the C++ core (llhttp, yyjson, ryu, custom serializer) and copies `_astraapi_core.so` to the package directory.

### Verify Build

```bash
python -c "import astraapi; print(astraapi.__version__)"
```

## Project Structure

```
astraapi/          # Python package (no Starlette dependency)
├── __init__.py              # Lazy imports
├── applications.py          # AstraAPI app class
├── _app_base.py             # AppBase with native middleware
├── _cpp_server.py           # Built-in server and protocol
├── _multiworker.py          # Multi-process worker manager
├── routing.py               # APIRouter and route matching
├── _middleware_impl.py      # Native middleware (CORS, GZip, etc.)
├── _websocket.py            # WebSocket support
├── _background.py           # Background tasks
├── _staticfiles.py          # Static file serving
├── _testclient.py           # Real C++ server test client
├── openapi/                 # OpenAPI schema generation
├── security/                # OAuth2, JWT, API key, Basic auth
└── dependencies/            # DI system

cpp_core/          # C++ core
├── CMakeLists.txt
├── include/               # Headers
├── src/                   # Implementation
│   ├── app.cpp            # Main dispatcher (~2800 lines)
│   ├── router.cpp         # Static hash + radix trie
│   ├── http_parser.cpp    # llhttp wrapper
│   ├── json_parser.cpp    # yyjson wrapper
│   ├── json_writer.cpp    # SIMD streaming serializer
│   ├── json_encoder.cpp   # Pydantic-aware encoder
│   ├── ws_frame_parser.cpp# WebSocket frame parser
│   ├── ws_ring_buffer.cpp # Ring buffer + direct FD writev
│   ├── middleware_engine.cpp # libdeflate compression
│   └── module.cpp         # Module init (~40 exports)
└── third_party/           # llhttp, yyjson, ryu

tests/             # Test suite (~190 top-level + ~250 tutorial)
docs_web/          # Documentation (VitePress)
```

## Running Tests

### Full Test Suite

```bash
pytest tests/ -x -q
```

Expected: `3730 passed, 4 skipped, 9 xfailed`

### Specific Test

```bash
pytest tests/test_routing.py -v
```

### With Coverage

```bash
pytest tests/ --cov=astraapi --cov-report=html
```

## Benchmarking

### Run Benchmarks

```bash
cd benchmarks
python server.py &
wrk -t2 -c10000 -d30s http://127.0.0.1:8002/
```

### Profile C++ Code

```bash
perf record -g python benchmarks/server.py
# In another terminal:
wrk -t2 -c10000 -d30s http://127.0.0.1:8002/
# Then:
perf report
```

## Code Style

### Python

- Follow PEP 8
- Use type hints
- Format with `ruff`

```bash
ruff check astraapi/
ruff format astraapi/
```

### C++

- C++20 standard
- Use `PyRef` RAII for all `PyObject*`
- Prefer `string_view` over `std::string` for zero-copy

## Making Changes

### 1. Create a Branch

```bash
git checkout -b feature/my-feature
```

### 2. Make Your Changes

- Write tests first (TDD encouraged)
- Update documentation
- Ensure backward compatibility

### 3. Run Tests

```bash
pytest tests/ -x
```

### 4. Run Benchmarks

Ensure no performance regressions:

```bash
cd benchmarks
python benchmark.py --before --after
```

### 5. Submit a PR

- Clear description of changes
- Link to related issues
- Include benchmark results if performance-related

## C++ Core Development

### Adding a New Feature

1. Add Python API in `astraapi/`
2. Add C++ implementation in `cpp_core/src/`
3. Update `cpp_core/include/astraapi/` if adding public headers
4. Update `cpp_core/CMakeLists.txt` if adding new source files
5. Build and test

### Common Patterns

**Calling Python from C++:**
```cpp
PyRef result(PyObject_CallNoArgs(endpoint));
if (!result) {
    return nullptr;  // Python exception is set
}
```

**Creating a Python tuple:**
```cpp
PyRef tuple(PyTuple_Pack(2, key, value));
```

**Error handling:**
```cpp
if (some_error) {
    PyErr_SetString(PyExc_ValueError, "Something went wrong");
    return nullptr;
}
```

## Documentation

Documentation is built with VitePress:

```bash
cd docs_web
bun install
bun run docs:dev
```

Edit files in `docs_web/` and see changes live at `http://localhost:5173`.

## Release Process

1. Update version in `pyproject.toml`
2. Update `CHANGELOG.md`
3. Tag release: `git tag v1.0.0`
4. Push tags: `git push origin v1.0.0`
5. GitHub Actions builds and publishes wheels

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
