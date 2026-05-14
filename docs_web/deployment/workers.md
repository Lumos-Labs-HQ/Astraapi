# Built-in Workers

AstraAPI includes a **custom multi-worker process manager**. You do not need gunicorn, uvicorn, hypercorn, or any external server.

## Why Built-in Workers?

External servers like gunicorn add layers of indirection:

| Problem | AstraAPI Solution |
|---------|-------------------|
| ASGI adapter overhead | Direct asyncio protocol — no ASGI translation |
| Process manager config | Zero config — `app.run(workers=N)` |
| Copy-on-Write inefficiency | Routes frozen before fork; COW shared pages |
| Cross-core cache thrashing | CPU affinity pinning per worker |
| Thundering herd | SO_REUSEPORT or SCM_RIGHTS fd dispatch |
| Crash recovery | Automatic restart with exponential backoff |

## How It Works

### Linux

**SO_REUSEPORT mode** (default when available):

```
Parent process
    |
    | fork() 4 workers
    |
    v
Worker 1 -----> binds socket with SO_REUSEPORT
Worker 2 -----> binds socket with SO_REUSEPORT
Worker 3 -----> binds socket with SO_REUSEPORT
Worker 4 -----> binds socket with SO_REUSEPORT

Kernel load-balances connections across all sockets
```

**Master-accept + SCM_RIGHTS fallback** (when SO_REUSEPORT unavailable):

```
Parent process
    | accept() on listening socket
    | round-robin fd to workers via Unix domain socket
    v
Worker 1 <---- recv fd via SCM_RIGHTS
Worker 2 <---- recv fd via SCM_RIGHTS
Worker 3 <---- recv fd via SCM_RIGHTS
Worker 4 <---- recv fd via SCM_RIGHTS
```

### Windows

```
Parent process
    |
    | spawn 4 subprocesses
    |
    v
Worker 1 -----> binds with SO_REUSEADDR
Worker 2 -----> binds with SO_REUSEADDR
Worker 3 -----> binds with SO_REUSEADDR
Worker 4 -----> binds with SO_REUSEADDR
```

Windows allows multiple processes to bind to the same address with SO_REUSEADDR.

## CPU Affinity

Workers are pinned to dedicated CPU cores:

- **Linux**: `os.sched_setaffinity(pid, {core_id})`
- **Windows**: `SetProcessAffinityMask(handle, mask)`

This eliminates cross-core cache thrashing and keeps worker data local to its L1/L2 cache.

## Supervision

The parent monitors workers via:
- **Linux**: `os.waitpid()`
- **Windows**: `proc.poll()`

Crashed workers are restarted with **exponential backoff** if they crash-loop (preventing restart storms).

## Configuration

```python
import os
from astraapi import AstraAPI

app = AstraAPI()

if __name__ == "__main__":
    workers = int(os.getenv("WORKERS", os.cpu_count() or 1))
    app.run(port=8000, workers=workers, backlog=65535)
```

| Parameter | Recommended | Notes |
|-----------|-------------|-------|
| `workers` | `os.cpu_count()` | More workers = more memory, not always more throughput |
| `backlog` | `65535` | Maximize kernel accept queue |

## Memory Usage

Each worker is an independent OS process:

| Workers | Memory (approx) |
|---------|----------------|
| 1 | ~50MB base |
| 4 | ~150MB total |
| 8 | ~280MB total |

Route tables are shared via Copy-on-Write, so additional workers only cost:
- Event loop + protocol pool (~10MB)
- Per-worker buffer pools (~2MB)
- Python interpreter overhead (~20MB)

## Graceful Shutdown

When the parent receives SIGINT or SIGTERM:

1. Parent sends shutdown signal to all workers
2. Workers stop accepting new connections
3. Workers finish in-flight requests
4. Workers close existing connections
5. Parent exits

## Reload

For development, use `reload=True` (single worker only):

```python
if __name__ == "__main__":
    app.run(port=8000, reload=True)
```

For production, restart the entire process tree:

```bash
kill -HUP $(pgrep -f "python main.py")
```

## systemd Service

```ini
# /etc/systemd/system/astraapi.service
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
