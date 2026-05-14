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

Output:
```
C++ HTTP server running on http://127.0.0.1:8000
Press Ctrl+C to stop
```

## app.run() Parameters

```python
app.run(
    host="127.0.0.1",       # Bind address (default: 127.0.0.1)
    port=8000,              # Bind port
    reload=False,           # Hot reload (development only)
    reload_dirs=None,       # Directories to watch for reload
    reload_includes=None,   # Extra glob patterns for reload
    reload_excludes=None,   # Paths to exclude from reload
    workers=1,              # Number of worker processes
    keep_alive_timeout=30.0,# HTTP keep-alive timeout in seconds
    max_body_size=0,        # Max request body size in bytes (0 = unlimited)
    max_body_size_kb=0,     # Alternative: body size in KB
    max_body_size_mb=0,     # Alternative: body size in MB
)
```

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
- **CPU affinity pinning**: Workers are pinned to dedicated cores to eliminate cross-core cache thrashing.
- **Supervision**: The parent monitors children and restarts crashed workers with exponential backoff.
- **Zero shared state**: No locks, no IPC during request processing.

## Auto-Tuning

When you call `app.run()`, AstraAPI automatically applies these optimizations:

- **RLIMIT_NOFILE** - Raises file descriptor limit to 65536
- **somaxconn** - Sets kernel listen queue to 65535
- **tcp_max_syn_backlog** - Sets TCP SYN backlog to 65535
- **tcp_tw_reuse** - Reuses TIME_WAIT sockets
- **tcp_fin_timeout** - Reduces FIN timeout to 10s
- **ip_local_port_range** - Expands ephemeral port range
- **TCP_FASTOPEN** - Reduces 1-RTT for new connections
- **TCP_DEFER_ACCEPT** - Delivers connections only when data arrives
- **TCP_NODELAY** - Disables Nagle's algorithm
- **GC disable + freeze** - Moves startup objects to permanent generation
- **Eager task factory** - Python 3.12+ async task optimization
- **C++ route warmup** - Warms instruction cache before first request
- **Buffer pool prewarm** - Pre-allocates thread-local response buffers
- **Protocol pool prewarm** - Pre-allocates 1024 protocol objects
- **CORS sync** - Automatically syncs CORS config to C++ core
- **libdeflate check** - Warns if libdeflate is not available

All of this happens automatically. You do not need to configure anything.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ASTRAAPI_MAX_CONNECTIONS` | `0` (unlimited) | Maximum concurrent connections |
| `ASTRAAPI_PREWARM_CONNS` | `1024` (single) / `512` (worker) | Protocol pool pre-warm size |
| `ASTRAAPI_CPU_AFFINITY` | `""` | CPU cores to pin to (e.g., `"0,1,2,3"`) |
| `ASTRAAPI_RT_PRIORITY` | `0` | Real-time SCHED_FIFO priority (Linux, requires root) |
| `ASTRAAPI_MAX_BODY_SIZE` | `0` (unlimited) | Max request body size in bytes |
| `ASTRAAPI_WS_PING_INTERVAL` | `30.0` | WebSocket ping interval in seconds |

## Event Loop

AstraAPI tries event loops in this order:

1. **uvloop** - fastest, Cython-based (Linux/macOS, already included)
2. **winloop** - uvloop equivalent for Windows (already included)
3. **asyncio** - Python's built-in loop (fallback)

No code changes needed. AstraAPI auto-detects and uses the best available loop.

## Keep-Alive

AstraAPI uses a **batch sweep** for HTTP keep-alive instead of per-connection timers:

- A background task runs every **10 seconds**
- Each protocol has a `_ka_needs_reset` flag set on every request
- During the sweep: reset flags update deadlines; expired connections are closed

This eliminates ~20,000 timer callbacks per second at 200K req/s.

The timeout is controlled by `keep_alive_timeout` (default: 30 seconds).

## Development Mode

```python
if __name__ == "__main__":
    app.run(port=8000, reload=True)
```

Auto-reloads the server when Python files change. Only works with `workers=1`.

Watch additional patterns:
```python
app.run(port=8000, reload=True, reload_includes=["*.yaml", "*.json"])
```

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

No gunicorn or uvicorn in the container. Just `python main.py`.

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

## systemd Service

```ini
[Unit]
Description=AstraAPI Server
After=network.target

[Service]
User=astraapi
Group=astraapi
WorkingDirectory=/opt/astraapi
Environment="PATH=/opt/astraapi/.venv/bin"
Environment="WORKERS=4"
ExecStart=/opt/astraapi/.venv/bin/python main.py
ExecReload=/bin/kill -s HUP $MAINPID
KillMode=mixed
TimeoutStopSec=5
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable astraapi
sudo systemctl start astraapi
sudo systemctl status astraapi
```
