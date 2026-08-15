# AstraAPI vs FastAPI Benchmark Suite

Comprehensive benchmark comparing AstraAPI and FastAPI with identical endpoints.
Uses Go-based load generator for maximum throughput with minimal client overhead.

## Structure

```
bench/
├── servers/
│   ├── astraapi_server.py   # AstraAPI server (1 worker, 1 core)
│   └── fastapi_server.py    # FastAPI + Uvicorn (1 worker, 1 core)
├── main.go                  # Go benchmark runner
├── go.mod
├── go.sum
├── run.sh                   # One-command full benchmark
└── README.md
```

## Workloads

| # | Name | Description |
|---|------|-------------|
| 1 | plaintext | `GET /plaintext` → "Hello, World!" |
| 2 | json | `GET /json` → `{"message":"Hello, World!"}` |
| 3 | single_query | `GET /db` → single JSON object |
| 4 | path_param | `GET /user/123` → path param extraction |
| 5 | query_params | `GET /search?q=hello&limit=10` → query parsing |
| 6 | post_small | `POST /items` → 100B JSON body parse + validate |
| 7 | post_large | `POST /upload` → 10KB JSON body |
| 8 | nested_json | `GET /nested` → deeply nested JSON response |
| 9 | headers | `GET /headers` → read/write multiple headers |
| 10 | websocket | `WS /ws/echo` → WebSocket echo roundtrip |

## Usage

```bash
cd bench
./run.sh
```

## Requirements

- Go 1.27rc2 (`~/go/bin/go1.27rc2`)
- Python 3.14+ with `uv`
- AstraAPI installed in the project venv
- FastAPI + Uvicorn installed (auto-installed by run.sh)
