<p align="center">
  <img src="https://raw.githubusercontent.com/Lumos-Labs-HQ/Astraapi/main/docs/public/icon.png" width="120" height="120" style="border-radius: 24px;" alt="AstraAPI">
</p>

<h1 align="center">AstraAPI</h1>

<p align="center">
  <b>FastAPI-compatible. C++ core. Built-in workers. No gunicorn needed.</b>
</p>

<p align="center">
  <a href="https://pypi.org/project/astraapi"><img src="https://img.shields.io/badge/pypi-v0.2.0-blue?logo=pypi" alt="PyPI"></a>
  <a href="https://github.com/Lumos-Labs-HQ/Astraapi/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="License"></a>
  <img src="https://img.shields.io/badge/python-3.12%2B-blue" alt="Python">
  <img src="https://img.shields.io/badge/C++-20-orange" alt="C++20">
</p>

---

**AstraAPI** is a drop-in replacement for [FastAPI](https://fastapi.tiangolo.com/) with a compiled C++20 core. Same decorators. Same Pydantic models. Same OpenAPI docs. But 5-10x faster — no external server required.

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def hello():
    return {"message": "Hello World"}

if __name__ == "__main__":
    app.run(port=8000, workers=4)
```

Output:
```
C++ HTTP server running on http://127.0.0.1:8000
Press Ctrl+C to stop
```

## Why AstraAPI?

| | FastAPI + Uvicorn | AstraAPI |
|---|---|---|
| HTTP parser | `httptools` (Python) | **llhttp** (C++, Node.js) |
| JSON | `orjson` | **yyjson** + SIMD serializer |
| Workers | gunicorn/uvicorn | **Built-in** fork/spawn |
| Keep-alive | Per-connection timers | **Batch sweep** |
| Compression | zlib | **libdeflate** (2-3x faster) |
| Throughput | ~45k req/s | **~236k req/s** |

## Install

```bash
pip install astraapi
```

Python 3.12+

## Quick Start

```bash
python -m astraapi --help
```

Or write a file:

```python
from astraapi import AstraAPI
from pydantic import BaseModel

app = AstraAPI()

class Item(BaseModel):
    name: str
    price: float

@app.get("/items/{item_id}")
def read_item(item_id: int, q: str | None = None):
    return {"item_id": item_id, "q": q}

@app.post("/items/")
def create_item(item: Item):
    return item

if __name__ == "__main__":
    app.run(port=8000, workers=4)
```

## Features

- **100% FastAPI compatible** — same `@app.get`, `Depends`, `BaseModel`, `Query`, `Path`, `File`, `UploadFile`, `WebSocket`, `BackgroundTasks`
- **Built-in multi-worker server** — `SO_REUSEPORT`, CPU affinity, auto-restart. No gunicorn.
- **C++ HTTP core** — llhttp parser, radix trie router, yyjson, SIMD string escape, ryu float formatting
- **Zero-copy transport** — direct `send()` for small responses, `writev()` for WebSockets
- **Native middleware** — CORS, GZip (libdeflate), TrustedHost, HTTPSRedirect, RateLimiting
- **Real TestClient** — starts actual C++ HTTP server on ephemeral port
- **Hot reload** — `app.run(reload=True)` watches files via `watchfiles`

## Documentation

Visit [https://astraapi.dev](https://astraapi.dev) or the [docs](./docs) folder.

## License

MIT — see [LICENSE](LICENSE).

<p align="center">
  Made with ⚡ by <a href="https://github.com/Lumos-Labs-HQ">Lumos Labs HQ</a>
</p>
