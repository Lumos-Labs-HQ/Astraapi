# Why AstraAPI?

AstraAPI is a **FastAPI-compatible** web framework for Python, rebuilt from the ground up with a **C++ core** and a **built-in multi-worker server** to deliver exceptional performance without sacrificing the developer experience you already know and love.

## The Philosophy

> Zero compromise on compatibility. Maximum performance under the hood. No external server required.

If you have used FastAPI, you already know AstraAPI. The same decorators, the same Pydantic models, the same dependency injection system, and the same automatic OpenAPI documentation. The only differences? Your endpoints run dramatically faster, and you do not need gunicorn or uvicorn.

## What Makes AstraAPI Different?

| Feature | FastAPI | AstraAPI |
|---------|---------|----------|
| Language | Pure Python | Python + C++ core |
| HTTP parser | h11 / httptools | llhttp (Node.js parser) + fast-path scanner |
| JSON parser | json / orjson | yyjson (~3GB/s) + custom SIMD serializer |
| JSON serializer | Python / orjson | Custom C++ streaming with SIMD escape + ryu floats |
| Compression | zlib | libdeflate (2-3x faster) + optional brotli |
| Route dispatch | Python dict lookup | C++ unordered_map (static) + radix trie (dynamic) |
| Server required | uvicorn / gunicorn | Built-in multi-worker |
| Workers | External process manager | Built-in fork/spawn with SO_REUSEPORT, CPU affinity |
| Keep-alive | Per-connection timers | Batch sweep every 10s |
| Test client | ASGI transport mock | Real C++ HTTP server on ephemeral port |
| Event loop | asyncio (default) | uvloop / winloop included by default |
| API compatibility | — | 100% FastAPI compatible |

## uvloop and orjson Included by Default

Unlike FastAPI, AstraAPI ships with performance-critical dependencies pre-installed:

```
uvloop>=0.22.1      # Linux/macOS — already in dependencies
winloop>=0.5.0      # Windows — already in dependencies
orjson>=3.11.5      # Fast JSON — already in dependencies
watchfiles>=1.1.1   # Hot reload — already in dependencies
```

No separate `pip install uvloop` needed. It is already there.

## When to Use AstraAPI

- High-throughput APIs — microservices, BFF layers, public APIs serving 100K+ req/s
- Latency-sensitive workloads — real-time bidding, gaming backends, financial tickers
- Resource-constrained deployments — get more out of each CPU core
- Existing FastAPI codebases — drop-in replacement with zero code changes
- Simplified ops — no gunicorn/uvicorn to configure, monitor, or tune

## Quick Comparison

### FastAPI
```python
from fastapi import FastAPI
import uvicorn

app = FastAPI()

@app.get("/")
def read_root():
    return {"message": "Hello World"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
```

### AstraAPI
```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def read_root():
    return {"message": "Hello World"}

if __name__ == "__main__":
    app.run(port=8000)
```

No uvicorn import. No server configuration. Just `app.run()`.

## Next Steps

- [Installation](./installation) — get AstraAPI running in under a minute
- [Quick Start](./quickstart) — build your first endpoint
- [Architecture](../architecture/) — understand how the C++ core works
