# Run the Server

AstraAPI includes a **built-in multi-worker server**. You do not need gunicorn, uvicorn, hypercorn, or any external server.

## Basic Usage

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def read_root():
    return {"Hello": "World"}

if __name__ == "__main__":
    app.run(port=8000)
```

## app.run() Options

```python
app.run(
    host="0.0.0.0",
    port=8000,
    workers=1,              # Number of process workers
    backlog=65535,          # Kernel listen backlog
    reload=False,           # Auto-reload on file change (dev only)
)
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `host` | `"0.0.0.0"` | Bind address |
| `port` | `8000` | Bind port |
| `workers` | `1` | Process workers (use os.cpu_count() for production) |
| `backlog` | `65535` | OS listen backlog |
| `reload` | `False` | Auto-reload on file change (dev only, single worker) |

## Multi-Worker Deployment

For production, use multiple workers to utilize all CPU cores:

```python
import os

if __name__ == "__main__":
    app.run(port=8000, workers=os.cpu_count() or 1)
```

### How Workers Work

AstraAPI's worker manager is entirely custom and built into the framework:

**Linux:**
- **SO_REUSEPORT mode** (preferred): Each worker binds its own socket. The kernel load-balances connections directly across workers.
- **Master-accept + SCM_RIGHTS fallback**: The parent process runs a centralized accept() thread and round-robins file descriptors to workers over Unix domain sockets using SCM_RIGHTS.

**Windows:**
- Workers bind independently using SO_REUSEADDR. No socket sharing or fd dispatch needed.

**All platforms:**
- Each worker is a fully independent OS process with its own GIL, memory space, and event loop.
- **CPU affinity pinning**: Workers are pinned to dedicated cores via os.sched_setaffinity() (Linux) or SetProcessAffinityMask (Windows) to eliminate cross-core cache thrashing.
- **Supervision**: The parent monitors children and restarts crashed workers with exponential backoff.
- **Zero shared state**: No locks, no IPC during request processing.

### Pre-Fork Model

Before forking, the parent calls:
1. `app._sync_routes_to_core()` — registers all routes with C++
2. `core_app.freeze_routes()` — freezes the route table as read-only
3. Pre-warms protocol pools and C++ buffer pools
4. Calls `gc.freeze()` — moves all startup objects to a permanent generation

Children inherit the frozen route table via **Copy-on-Write (COW)** pages.

## Event Loop Priority

AstraAPI tries event loops in this order:

1. **uvloop** — fastest, Cython-based (Linux/macOS)
2. **winloop** — uvloop equivalent for Windows
3. **asyncio** — Python's built-in loop (fallback)

No code changes needed — AstraAPI auto-detects and uses the best available loop.

```bash
pip install uvloop  # Strongly recommended for production
```

## TCP Optimizations

AstraAPI applies several low-level TCP optimizations automatically:

| Option | Purpose |
|--------|---------|
| `TCP_NODELAY` | Disables Nagle's algorithm for low latency |
| `TCP_QUICKACK` | Re-armed per read to reduce ACK latency |
| `TCP_FASTOPEN` | Reduces 1-RTT for new connections |
| `TCP_DEFER_ACCEPT` | Do not wake worker until data arrives |
| `TCP_CORK` | Batches segments during WebSocket write bursts |

## Keep-Alive

AstraAPI uses a **batch sweep** for HTTP keep-alive instead of per-connection timers:

- A background task runs every **10 seconds**
- Each protocol has a `_ka_needs_reset` flag set on every request
- During the sweep: reset flags update deadlines; expired connections are closed

This eliminates **~20,000 timer callbacks per second** at 200K req/s.

## Pre-Warming

Before accepting connections, AstraAPI pre-warms multiple layers:

1. **Protocol Object Pool** — 1024 objects (single-process) or 512 per worker
2. **C++ Buffer Pool** — thread-local response buffers
3. **Cached Refs** — pre-imports modules and interns strings
4. **Route Warmup** — exercises parse, route, serialize to warm instruction cache
5. **GC Freeze** — moves startup objects to permanent generation

## Development Mode

```python
if __name__ == "__main__":
    app.run(port=8000, reload=True)
```

Auto-reloads the server when Python files change. Only works with `workers=1`.

## Docker

```dockerfile
FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install -r requirements.txt

COPY . .
EXPOSE 8000

CMD ["python", "main.py"]
```

No gunicorn or uvicorn in the container — just `python main.py`.

```python
# main.py
import os
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def root():
    return {"ok": True}

if __name__ == "__main__":
    workers = int(os.getenv("WORKERS", os.cpu_count() or 1))
    app.run(port=8000, workers=workers)
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `ASTRAAPI_PREWARM_CONNS` | Protocol pool pre-warm size (default: 1024 single / 512 per worker) |

## Performance Mode

For benchmarking or maximum throughput:

```python
import uvloop
uvloop.install()

app.run(
    port=8000,
    workers=6,
    backlog=65535,
)
```

This configuration:
- Uses uvloop for faster event loop
- Pre-warms protocol objects to eliminate allocation at high concurrency
- Freezes GC to prevent collection pauses
- Sets a large kernel backlog to handle connection bursts
