# Optimization Guide

Get the most out of AstraAPI with these production tuning tips.

## 1. Use uvloop

```bash
pip install uvloop
```

AstraAPI automatically detects and uses uvloop. No code changes needed.

uvloop replaces Python's default asyncio event loop with a Cython implementation. It provides:
- Faster socket polling
- Lower timer overhead
- More efficient connection handling

**Impact:** +15-30% throughput

## 2. Use Multiple Workers

```python
import os

if __name__ == "__main__":
    app.run(port=8000, workers=os.cpu_count() or 1)
```

AstraAPI's built-in worker manager is more efficient than external process managers because:
- Routes are frozen and shared via Copy-on-Write
- No inter-process locks or shared memory
- CPU affinity pinning eliminates cache thrashing

**Impact:** Linear scaling up to CPU core count

## 3. Increase Kernel Backlog

```python
app.run(port=8000, backlog=65535)
```

The kernel listen backlog controls how many pending connections are queued. The default (128) is too small for high-throughput servers.

**Impact:** Fewer dropped connections under burst load

## 4. Tune TCP Settings

System-level tuning for high-throughput servers:

```bash
# /etc/sysctl.conf
net.ipv4.tcp_tw_reuse = 1
net.ipv4.ip_local_port_range = 1024 65535
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.tcp_fastopen = 3
net.ipv4.tcp_notsent_lowat = 16384
```

Apply with `sudo sysctl -p`.

## 5. Response Cache Design

Return hashable, JSON-serializable data to hit the response cache:

```python
# Cacheable
def get_config():
    return {"version": "1.0", "debug": False}  # Same dict every time -> cache hit

# Not cacheable
def get_random():
    import random
    return {"random": random.randint(1, 100)}  # Different every time -> cache miss
```

## 6. Avoid Large Response Models

Pydantic model serialization for large lists is slower than raw dicts:

```python
# Fast — C++ serializes dict directly
@app.get("/items")
def list_items():
    return [{"id": i} for i in range(100)]

# Slower — Pydantic converts each item first
@app.get("/items", response_model=list[ItemOut])
def list_items():
    return items
```

Use `response_model` when you need validation/filtering. Skip it for internal APIs where you control the data.

## 7. Async Endpoints

For CPU-bound work, use sync endpoints. For I/O-bound work, use async:

```python
# CPU-bound — sync is fine, GIL released during socket ops
def calculate(data: DataModel):
    return heavy_computation(data)

# I/O-bound — async allows other requests during await
async def fetch_data():
    result = await db.fetch("SELECT * FROM items")
    return result
```

## 8. Static Files

Serve static files via CDN or Nginx, not AstraAPI:

```nginx
# nginx.conf
location /static/ {
    alias /var/www/static/;
    expires 1y;
    add_header Cache-Control "public, immutable";
}

location / {
    proxy_pass http://astraapi_backend;
}
```

## 9. Connection Keep-Alive

Keep-alive is enabled by default. The batch sweep runs every 10 seconds. Tune the timeout:

```python
# Per-connection timeout is controlled by the protocol
# The batch sweep interval is fixed at 10s
```

Longer timeouts reduce connection churn for API clients that make frequent requests.

## 10. Monitor with Metrics

Add timing middleware to track real performance:

```python
@app.middleware("http")
async def metrics(request, call_next):
    import time
    start = time.perf_counter()
    response = await call_next(request)
    duration = time.perf_counter() - start
    
    # Export to Prometheus, StatsD, etc.
    METRICS_HISTOGRAM.observe(duration)
    
    return response
```

## 11. Profile Your App

Use `py-spy` or `perf` to find real bottlenecks:

```bash
# Sample Python stack traces
sudo py-spy top --pid $(pgrep -f "python main.py")

# Profile C++ core
perf record -g -p $(pgrep -f "python main.py")
perf report
```

## Quick Checklist

- [ ] uvloop installed
- [ ] Multiple workers matching CPU cores
- [ ] Backlog increased to 65535
- [ ] Kernel TCP settings tuned
- [ ] Static files served by CDN/Nginx
- [ ] Response cache utilized for static responses
- [ ] Pydantic models kept lean for hot paths
- [ ] Monitoring middleware in place
