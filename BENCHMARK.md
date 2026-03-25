# FastAPI-Rust Benchmark Report

**Date:** 2026-03-25  
**Platform:** WSL2 · Linux 5.15.153.1-microsoft-standard-WSL2  
**CPU:** Intel i5-11400H @ 2.70GHz · 6 cores / 12 threads (HT) · AVX-512  
**RAM:** 8 GB (WSL2 allocation)  
**Tool:** [bombardier](https://github.com/codesenberg/bombardier) `-c100` (100 concurrent connections)  
**Method:** 3-run average, server warmed up before measurement, each server isolated (no overlap)

---

## Versions

| Runtime | Version |
|---|---|
| Python | 3.14.3 |
| Bun | 1.3.9 |
| Axum | 0.8.8 / Tokio 1.50.0 |

---

## HTTP Benchmark — `GET /` returning `{"message":"Hello World"}`

### Single Worker / Single Process

| Framework | Req/s | p50 | p95 | p99 | RAM idle | RAM load | CPU (load) |
|---|---:|---:|---:|---:|---:|---:|---:|
| **FastAPI-Rust async** (1w) | **121,616** | 421µs | 0.92ms | 1.31ms | 85 MB | 85 MB | ~100% |
| **FastAPI-Rust sync** (1w) | **136,611** | 419µs | 0.92ms | 1.23ms | 85 MB | 85 MB | ~100% |
| Hono/Bun | 118,145 | 742µs | 1.95ms | 2.95ms | 61 MB | 68 MB | ~100% |
| Axum/Rust | 200,522 | 463µs | 0.87ms | 1.19ms | 28 MB | 32 MB | ~100% |

> FastAPI-Rust **beats Hono/Bun by ~3% (async) and ~16% (sync)** on a single worker.  
> Axum leads due to multi-threaded Tokio runtime vs single-threaded Python event loop.

---

### Multi-Worker (FastAPI-Rust `workers=2` vs competitors)

| Framework | Config | Req/s | p50 | p95 | p99 | RAM idle | RAM load |
|---|---|---:|---:|---:|---:|---:|---:|
| **FastAPI-Rust async** | **2 workers** | **222,989** | 430µs | 0.92ms | 1.36ms | 173 MB | 173 MB |
| **FastAPI-Rust sync** | **2 workers** | **236,648** | 379µs | 805µs | 1.19ms | 173 MB | 173 MB |
| Hono/Bun | 1 process | 118,145 | 742µs | 1.95ms | 2.95ms | 61 MB | 68 MB |
| Axum/Rust | multi-thread | 200,522 | 463µs | 0.87ms | 1.19ms | 28 MB | 32 MB |

> FastAPI-Rust `workers=2` **beats Axum by 11% (async) and 18% (sync)**, and beats Hono/Bun by **89%**.  
> Change: `app.run(host="127.0.0.1", port=8002, workers=2)`

---

## WebSocket Benchmark — Echo, 60k messages

| Framework | 50 conns msg/s | 200 conns msg/s |
|---|---:|---:|
| **FastAPI-Rust** | **21,454** 🏆 | **26,769** 🏆 |
| Hono/Bun | 17,731 | 21,632 |
| Axum/Rust | 19,584 | 20,844 |

> FastAPI-Rust beats both at all concurrency levels. WS is single-worker only (stateful connections).

---

## Resource Usage Summary

| Framework | Config | RAM idle | RAM peak load | Notes |
|---|---|---:|---:|---|
| Axum/Rust | multi-thread | 28 MB | 32 MB | Tokio thread pool, zero GC |
| Hono/Bun | 1 process | 61 MB | 68 MB | V8 JIT + JS runtime |
| **FastAPI-Rust** | **1 worker** | **85 MB** | **85 MB** | Python + C++ extension |
| **FastAPI-Rust** | **2 workers** | **173 MB** | **173 MB** | 2× forked Python processes |

> Memory is stable under load (no growth) — the C++ core handles all allocations in the hot path.  
> RAM is flat because the C++ ring buffers are pre-allocated and Python GC has nothing to collect per-request.

---

## Benchmark Stability Note

`wrk -t2` is **unreliable on WSL2** for single-threaded servers — its 2 threads compete with the server for CPU cores, causing ±30–50% variance. Use `bombardier` (single-threaded client) or pin with `taskset`:

```bash
# Stable measurement: pin server to core 0, bombardier uses other cores
taskset -c 0 .venv/bin/python ns_ws.py &
bombardier -c100 -n200000 http://127.0.0.1:8002/async
```

With `bombardier`, variance is **±3%** across runs.

---

## How to Reproduce

```bash
# FastAPI-Rust 1 worker
.venv/bin/python ns_ws.py &
bombardier -c100 -n200000 -l http://127.0.0.1:8002/async

# FastAPI-Rust 2 workers (edit ns_ws.py: workers=2)
.venv/bin/python ns_ws.py &
bombardier -c100 -n400000 -l http://127.0.0.1:8002/async

# Hono/Bun
cd ben_test/bun_Test && bun ws.ts &
bombardier -c100 -n200000 -l http://127.0.0.1:3000/

# Axum/Rust
ben_test/rust_test/target/release/rust_test &
bombardier -c100 -n200000 -l http://127.0.0.1:4000/
```

---

## Key Optimizations Applied

| # | Optimization | Impact |
|---|---|---|
| 1 | Fast-path GET/HEAD parser (bypass llhttp) | ~0.3µs/req saved |
| 2 | `METH_FASTCALL` for WS C++ functions | Eliminates arg tuple alloc per WS frame |
| 3 | `PyObject_CallMethodOneArg` + interned strings for WS | Eliminates format string alloc |
| 4 | Direct `Future._state` identity check | Replaces `.done()` method call |
| 5 | Gated query string / header parse (`_needs_req_ctx`) | ~0.74µs/req saved on HTTP |
| 6 | `_assert_connected` off fast path in WebSocket | Eliminates branch on every send/recv |
| 7 | `_cpp_ws` fast path in `WebSocket.__init__` | Bypasses ASGI dict overhead |
| 8 | Echo detection disabled | Removes per-message check |
