---
layout: home

hero:
  name: AstraAPI
  text: FastAPI. Unchained.
  tagline: The same developer experience you love, powered by a C++ core with built-in multi-worker server. No gunicorn. No uvicorn. Just speed.
  image:
    src: /hero-illustration.svg
    alt: AstraAPI
  actions:
    - theme: brand
      text: Get Started
      link: /guide/getting-started
    - theme: alt
      text: View Benchmarks
      link: /performance/benchmarks
    - theme: alt
      text: Architecture
      link: /architecture/

features:
  - icon: 🚀
    title: Blazing Fast
    details: C++ HTTP core with llhttp parser, yyjson, SIMD-accelerated serialization, and zero-copy transport writes. 200K+ req/s on a single core.
  - icon: 🏗️
    title: Built-in Workers
    details: Multi-process server with SO_REUSEPORT, CPU affinity pinning, and automatic worker restart. No gunicorn or uvicorn needed.
  - icon: 🐍
    title: 100% FastAPI Compatible
    details: Drop-in replacement for FastAPI. Same decorators, same Pydantic models, same dependency injection. Zero migration cost.
  - icon: ⚡
    title: Native Async
    details: Built on Python asyncio with uvloop support. Async and sync endpoints are both first-class. Seamless WebSocket & streaming support.
  - icon: 🔒
    title: Production Ready
    details: OpenAPI, automatic validation, OAuth2, JWT, CORS, GZip, static files, background tasks, and dependency injection — all included.
  - icon: 🧪
    title: Real C++ Test Client
    details: TestClient spins up the actual C++ HTTP server on an ephemeral port — testing the real code path, not a mocked ASGI transport.
---

<div style="text-align: center; padding: 2rem 0;">

## Hello, AstraAPI

```python
from astraapi import AstraAPI
from pydantic import BaseModel

app = AstraAPI()

class Item(BaseModel):
    name: str
    price: float

@app.get("/")
def read_root():
    return {"Hello": "World"}

@app.post("/items/{item_id}")
def create_item(item_id: int, item: Item):
    return {"item_id": item_id, **item.model_dump()}

if __name__ == "__main__":
    app.run(port=8000)
```

</div>

<style>
.VPFeature {
  background: var(--vp-c-bg-soft);
  border-radius: 12px;
  padding: 24px;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}
.VPFeature:hover {
  transform: translateY(-4px);
  box-shadow: 0 8px 24px rgba(0,0,0,0.08);
}
.VPFeature .icon {
  font-size: 2rem;
  margin-bottom: 0.5rem;
}
.VPFeatures {
  margin-top: 3rem !important;
}
</style>
