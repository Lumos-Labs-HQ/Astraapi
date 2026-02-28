

import time
import os
import sys
from datetime import datetime, timezone

from fastapi import FastAPI

app = FastAPI(title="FastAPI + Core")


def _fmt(bytes_val: int) -> str:
    """Format bytes into a human-readable string."""
    for unit in ("B", "KB", "MB", "GB"):
        if abs(bytes_val) < 1024:
            return f"{bytes_val:.2f} {unit}"
        bytes_val /= 1024
    return f"{bytes_val:.2f} TB"


def _mem(b: int) -> dict:
    """Return a {bytes, human} pair matching the Go MemoryInfo struct."""
    return {"bytes": b, "human": _fmt(b)}


def _read_proc_self() -> dict:
    """Read /proc/self/status for VmRSS, VmSize etc. (Linux)."""
    info = {}
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith(("VmRSS:", "VmSize:", "VmData:")):
                    key, val = line.split(":", 1)
                    info[key] = int(val.strip().split()[0]) * 1024  # kB → bytes
    except (FileNotFoundError, PermissionError):
        pass
    return info


def _humanize_time(seconds: float) -> str:
    """Turn elapsed seconds into a human-friendly string."""
    if seconds < 60:
        return f"{int(seconds)} seconds ago"
    elif seconds < 3600:
        return f"{int(seconds // 60)} minutes ago"
    elif seconds < 86400:
        return f"{int(seconds // 3600)} hours ago"
    else:
        return f"{int(seconds // 86400)} days ago"


@app.get("/")
def root():
    return {"message": "Hello World"}

@app.get("/async")
async def root():
    return {"message": "Hello World"}


@app.get("/hlth")
def health():
    elapsed = time.time() - _startup_time

    # Memory via /proc
    current_alloc = peak_alloc = 0
    proc = _read_proc_self()
    rss = proc.get("VmRSS", 0)
    vms = proc.get("VmSize", 0)
    heap_data = proc.get("VmData", 0)

    return {
        "uptime": _humanize_time(elapsed),
        "version": sys.version.split()[0],
        "environment": os.getenv("APP_ENV", "development"),
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "memory": {
            "alloc": _mem(current_alloc),
            "total_alloc": _mem(peak_alloc),
            "sys": _mem(vms),
            "heap_alloc": _mem(rss),
            "heap_sys": _mem(heap_data),
        },
    }


_startup_time = time.time()

if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8002
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    
    print(f"🚀 Starting server at {host}:{port}...")
    app.run(host=host, port=port, workers=2)
    
