# Multi-Worker Architecture

## Overview

Master-accept + SCM_RIGHTS fd dispatch (Node.js cluster SCHED_RR pattern).
Zero thundering herd, zero shared state, zero GIL contention between workers.

```
                         ┌──────────────┐
                         │    CLIENTS   │
                         └──────┬───────┘
                                │
                     ┌──────────▼──────────┐
                     │   LISTEN SOCKET     │
                     │   backlog = 65535   │
                     └──────────┬──────────┘
                                │
                     ┌──────────▼──────────┐
                     │   MASTER PROCESS    │
                     │   (accept thread)   │
                     │                     │
                     │  accept() ────┐     │
                     │  round-robin  │     │
                     │  idx % N      │     │
                     └───────────────┼─────┘
                                    │
           sendmsg(SCM_RIGHTS)      │
      ┌──────────┬──────────┬───────┴──┬──────────┐
      ▼          ▼          ▼          ▼          ▼
 ┌─────────┐┌─────────┐┌─────────┐┌─────────┐┌─────────┐
 │Worker 0 ││Worker 1 ││Worker 2 ││Worker 3 ││Worker N │
 │ uvloop  ││ uvloop  ││ uvloop  ││ uvloop  ││ uvloop  │
 │         ││         ││         ││         ││         │
 │recvmsg()││recvmsg()││recvmsg()││recvmsg()││recvmsg()│
 │   ↓     ││   ↓     ││   ↓     ││   ↓     ││   ↓     │
 │ C++ HTTP││ C++ HTTP││ C++ HTTP││ C++ HTTP││ C++ HTTP│
 │ parse → ││ parse → ││ parse → ││ parse → ││ parse → │
 │ route → ││ route → ││ route → ││ route → ││ route → │
 │ respond ││ respond ││ respond ││ respond ││ respond │
 └─────────┘└─────────┘└─────────┘└─────────┘└─────────┘
  Separate   Separate   Separate   Separate   Separate
  GIL+mem    GIL+mem    GIL+mem    GIL+mem    GIL+mem
```

## Startup Sequence

```
run_multiworker(app, host, port, workers=N)
│
├─ 1. Raise RLIMIT_NOFILE → 65535
├─ 2. Tune sysctl (somaxconn, tcp_max_syn_backlog, etc.)
├─ 3. Sync routes to C++ CoreApp
├─ 4. Freeze route trie (read-only for COW sharing)
├─ 5. Create listen socket (backlog=65535)
├─ 6. Create N Unix socketpairs (one per worker)
│
├─ 7. Fork N workers
│     └─ Each child:
│         ├─ Closes listen socket (master handles accept)
│         ├─ Closes other workers' socketpair ends
│         ├─ Sets uvloop event loop policy
│         ├─ Calls run_server(unix_sock=child_end)
│         │     ├─ gc.disable()
│         │     ├─ Pre-warm protocol pool (256 protocols)
│         │     ├─ gc.freeze() (startup objects → permanent gen)
│         │     └─ add_reader(unix_sock) → _on_readable callback
│         └─ Event loop runs forever
│
├─ 8. Start master accept thread
│     └─ Blocking accept() loop → sendmsg(SCM_RIGHTS) round-robin
│
└─ 9. Monitor workers (restart on crash, graceful shutdown on signal)
```

## Request Flow (Multi-Worker)

```
1. CLIENT ──TCP SYN──→ LISTEN SOCKET

2. MASTER accept thread:
   accept() → fd=45
   round-robin → worker 2
   sendmsg(parent_socks[2], SCM_RIGHTS, fd=45)
   close(fd=45)

3. WORKER 2 event loop:
   _on_readable() fires
   ├─ recvmsg(unix_sock) → extract fd=45 from ancdata
   ├─ socket.fromfd(45) → Python socket object
   ├─ os.close(45)       → close dup, reference in socket obj
   └─ create_task(connect_accepted_socket(protocol_factory, sock))

4. connection_made(transport):
   ├─ Set TCP_NODELAY, TCP_QUICKACK
   ├─ Set write_buffer_limits(high=64KB, low=16KB)
   └─ Await data_received()

5. data_received(data):  ← HTTP request bytes
   └─ core.handle_http(http_buf, transport, offset, sock_fd)
       │
       ├─ C++ FAST PATH (sync endpoint):
       │   ├─ llhttp parse (zero-copy StringView)
       │   ├─ Route match (matchit trie)
       │   ├─ Param extract (O(1) hash maps, interned field names)
       │   ├─ CORS / trusted host checks
       │   ├─ Call endpoint (PyObject_CallNoArgs)
       │   ├─ JSON serialize (yyjson + ryu floats)
       │   ├─ Compress (gzip/brotli, GIL released)
       │   └─ transport.write(response)  ← ~1μs total
       │
       └─ ASYNC PATH (returns tuple to Python):
           ├─ C++ pre-drives coroutine via PyIter_Send()
           ├─ Python completes remaining awaits
           ├─ C++ builds response (build_response_from_any)
           └─ transport.write(response)
```

## Memory Per Worker

### Baseline (~20 MB)

| Component                      | Size   | Notes                              |
| ------------------------------ | ------ | ---------------------------------- |
| Protocol pool (256 pre-warmed) | ~16 MB | 256 × 65KB each                    |
| Event loop (uvloop/libuv)      | ~1 MB  | epoll tables, timer heaps          |
| Python interpreter             | ~3 MB  | Code pages shared via COW          |
| C++ CoreApp + route trie       | ~0 MB  | COW shared, read-only after freeze |

### Per Active Connection (~65 KB)

| Component                      | Size                             |
| ------------------------------ | -------------------------------- |
| Write buffer (high water mark) | 64 KB (worker) / 128 KB (single) |
| Protocol object (slots)        | ~1 KB                            |
| C++ HTTP buffer                | varies                           |

### Under Load (10K connections, 12 workers)

```
Per worker:  ~833 connections × 65 KB ≈ 54 MB + 20 MB baseline ≈ 74 MB
Total:       12 workers × 74 MB + master ≈ 900 MB
```

## Copy-On-Write (COW) Memory Sharing

### Shared After Fork (read-only, zero page faults)

- Frozen route trie (path strings, handler refs, param specs)
- Python code objects (all imported modules)
- C++ extension module (`_fastapi_core`)
- Interned strings (HTTP method names, field names)

### Private Per Worker (separate copies)

- Event loop state (uvloop handles, callback queues)
- Protocol pool (acquired/released independently)
- Active connection set + pending tasks
- GC state (frozen independently)

## GC Strategy

```
Startup:  gc.disable() → pre-warm → gc.collect(0,1,2) → gc.freeze()
Runtime:  Refcounting only (no periodic collection)
Idle:     gc.collect(1) every 600s when active_count == 0
Safety:   gc.collect(0) if RSS grows >100MB (gen0 only, <1ms)
```

- Startup objects in permanent generation → never scanned
- Per-request objects (coroutines, dicts) freed by refcount
- Avoids 60-70ms gen2 stalls measured at 88K+ req/s

## Worker Monitoring

- Supervisor loop checks `os.waitpid()` every 200ms
- Dead workers automatically restarted (new fork + new socketpair)
- Graceful shutdown: SIGTERM → 5s grace → SIGKILL
- Listen socket closed to stop accept thread

## Platform Support

| Feature       | Linux/macOS                  | Windows                       |
| ------------- | ---------------------------- | ----------------------------- |
| Process model | `os.fork()`                  | `subprocess.Popen()`          |
| fd dispatch   | `sendmsg(SCM_RIGHTS)`        | `socket.share(pid)`           |
| fd receive    | `recvmsg()` + `add_reader()` | Reader thread + `fromshare()` |
| Event loop    | uvloop                       | winloop / asyncio             |
| COW sharing   | Yes (fork inherits)          | No (separate processes)       |

## Configuration

| Env Variable              | Default       | Description               |
| ------------------------- | ------------- | ------------------------- |
| `FASTAPI_MAX_CONNECTIONS` | 0 (unlimited) | Per-worker connection cap |
| Workers param             | 1             | `app.run(workers=N)`      |

## Benchmarks (12 cores, WSL2, 10 workers)

| Benchmark              | Req/s       | Latency |
| ---------------------- | ----------- | ------- |
| wrk -t4 -c1000         | **434,309** | 3.4 ms  |
| wrk -t8 -c10000        | **341,981** | 27.4 ms |
| wrk -t4 -c10000        | **329,376** | 15.6 ms |
| autocannon -w4 -c10000 | **196,851** | 100 ms  |

---

## Comparison: This Architecture vs Gunicorn + Uvicorn

```
THIS PROJECT                          GUNICORN + UVICORN
────────────────────────────────────  ────────────────────────────────────
Master accepts ALL connections        Each worker calls accept() on
→ sendmsg(SCM_RIGHTS) to workers     shared socket → thundering herd
→ Zero thundering herd               → OS picks "random" worker

C++ fast path:                        Full Python stack:
  llhttp parse (zero-copy)              httptools parse (C + Python callbacks)
  matchit trie routing                  Starlette regex routing
  O(1) param extraction                 Python dict construction
  yyjson serialize (~3GB/s)             json.dumps / orjson
  gzip/brotli (GIL released)            Python gzip (GIL held)
  ~1μs per sync request                 ~50-100μs per request

gc.freeze() + refcount only           Standard Python GC
  gen0 scan <0.1ms                      gen2 scans: 60-70ms stalls
  No runtime collections                Periodic collections under load

Protocol pool (32K, reuse)            New protocol per connection
  _reinit() resets 15 fields            __init__() allocates everything
  Zero GC pressure                      GC tracks all allocations
```

### Numbers Side by Side

| Metric                      | This Project          | Gunicorn + Uvicorn              |
| --------------------------- | --------------------- | ------------------------------- |
| **Throughput (10 workers)** | **434,000 req/s**     | ~30,000-50,000 req/s            |
| **Sync endpoint latency**   | ~1 μs (C++)           | ~50-100 μs (Python)             |
| **Memory per worker**       | ~20 MB                | ~50-80 MB                       |
| **Memory at 10K conns**     | ~70 MB/worker         | ~100+ MB/worker                 |
| **Accept model**            | Master round-robin    | Shared socket (thundering herd) |
| **GC pauses under load**    | 0 ms                  | 60-70 ms (gen2)                 |
| **HTTP parsing**            | C++ llhttp, zero-copy | httptools + Python callbacks    |
| **JSON serialization**      | C++ yyjson + ryu      | Python json/orjson              |
| **Compression**             | C++ (GIL released)    | Python (GIL held)               |
| **Protocol reuse**          | Pool of 32K           | New per connection              |
| **Speedup**                 | **~8-14x faster**     | baseline                        |
