# Benchmarks

AstraAPI is designed for speed. These benchmarks compare AstraAPI against FastAPI and other popular Python frameworks under realistic conditions.

## Test Environment

| Component | Specification |
|-----------|--------------|
| CPU | 11th Gen Intel Core i5-11400H (6c/12t) |
| RAM | 16GB DDR4 |
| OS | Linux 6.8 |
| Python | 3.14.4 |
| Event Loop | uvloop |
| Tool | wrk 4.2.0 |

## Hello World Endpoint

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def read_root():
    return {"Hello": "World"}

if __name__ == "__main__":
    app.run(port=8002)
```

### Results

| Framework | Server | c=100 | c=1000 | c=10000 |
|-----------|--------|-------|--------|---------|
| **AstraAPI (sync)** | **Built-in** | **~236k** | **~150k** | **~101k** |
| **AstraAPI (async)** | **Built-in** | **~235k** | **~145k** | **~95k** |
| FastAPI | uvicorn | ~45k | ~42k | ~38k |
| FastAPI | hypercorn | ~38k | ~35k | ~32k |
| Starlette | uvicorn | ~55k | ~48k | ~42k |
| Flask | gunicorn | ~12k | ~11k | ~10k |
| Django | gunicorn | ~8k | ~7k | ~6k |

> All numbers in requests/second. Higher is better.

AstraAPI uses its **built-in server** — no external server process needed. FastAPI and Starlette require uvicorn or hypercorn, which adds overhead.

## JSON Serialization

```python
@app.get("/json")
def json_endpoint():
    return {
        "id": 1,
        "name": "Test Item",
        "tags": ["python", "fast", "api"],
        "metadata": {"version": "1.0", "active": True}
    }
```

| Framework | c=100 | c=10000 |
|-----------|-------|---------|
| **AstraAPI** | **~210k** | **~88k** |
| FastAPI + orjson | ~52k | ~45k |
| FastAPI (default) | ~38k | ~32k |

AstraAPI's custom C++ streaming serializer with SIMD string escaping and ryu float formatting outperforms even orjson.

## Response Cache Impact

```python
@app.get("/config")
def get_config():
    return {"version": "1.0.0", "debug": False, "features": ["auth", "cache"]}
```

| Mode | Throughput (c=10000) | Response Time |
|------|---------------------|---------------|
| Cache disabled | ~72k req/s | ~12us |
| Cache enabled | ~95k req/s | ~2us |
| **Improvement** | **+32%** | **-83%** |

## Async vs Sync

```python
@app.get("/sync")
def sync_endpoint():
    return {"mode": "sync"}

@app.get("/async")
async def async_endpoint():
    return {"mode": "async"}
```

| Concurrency | Sync | Async | Gap |
|-------------|------|-------|-----|
| c=100 | ~236k | ~235k | 0% |
| c=1000 | ~150k | ~145k | 3% |
| c=10000 | ~101k | ~95k | 6% |

The async gap at extreme concurrency is due to CPython coroutine object allocation (~2us overhead). For endpoints that actually await I/O, this overhead is negligible.

## WebSocket Performance

| Metric | Value |
|--------|-------|
| Connections per process | 50,000+ |
| Memory per connection | ~12KB |
| Message latency (p99) | <1ms |
| Broadcast throughput | 500K msg/s |

WebSocket direct FD writev() with SIMD unmasking provides exceptional throughput.

## Memory Usage

| Framework | Server | Idle RSS | 10K Connections | Per Connection |
|-----------|--------|----------|-----------------|----------------|
| **AstraAPI** | Built-in | 35MB | 155MB | ~12KB |
| FastAPI | uvicorn | 42MB | 285MB | ~24KB |
| Starlette | uvicorn | 38MB | 220MB | ~18KB |

AstraAPI's protocol pooling and slot-based objects reduce per-connection memory by ~40%.

## How to Reproduce

```bash
# Terminal 1: Start server
python benchmarks/server.py

# Terminal 2: Run wrk
wrk -t2 -c10000 -d30s --latency http://127.0.0.1:8002/
```

### Server Script

```python
import uvloop
from astraapi import AstraAPI

uvloop.install()
app = AstraAPI()

@app.get("/")
def root():
    return {"Hello": "World"}

if __name__ == "__main__":
    app.run(port=8002, workers=1)
```

## Benchmarking Tips

1. **Use uvloop** — significant event loop improvement
2. **Pre-warm pool** — protocol objects pre-allocated at startup
3. **Warm up** — ignore the first 5 seconds of any benchmark
4. **Check client limits** — `wrk` itself can become the bottleneck at c=10000; use multiple instances or `oha`
5. **Monitor thermals** — sustained load causes CPU throttling; ensure adequate cooling

## Real-World Performance

Synthetic benchmarks measure the framework, not your app. In practice, your database, cache, and external APIs are usually the bottleneck. AstraAPI's value is:

- **Lower latency at the tail** — P99 stays flat under load
- **More headroom** — same hardware handles 2-5x more traffic
- **Cost savings** — fewer servers needed for the same throughput
- **Simpler ops** — no external server process to tune or monitor
