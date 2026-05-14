# CORS

Enable Cross-Origin Resource Sharing (CORS) so browsers can make requests to your API from different origins.

AstraAPI's CORS is **native** (no Starlette dependency) and syncs its configuration directly to the C++ core. This means CORS preflight responses are handled entirely in C++ — zero Python overhead.

## Basic CORS

```python
from astraapi import AstraAPI, CORSMiddleware

app = AstraAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:3000", "https://myapp.com"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
```

## Allow All Origins

```python
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)
```

> Warning: `allow_origins=["*"]` with `allow_credentials=True` is a security risk. Do not use this in production.

## Specific Origins

```python
origins = [
    "http://localhost:3000",
    "http://localhost:8080",
    "https://app.example.com",
]

app.add_middleware(
    CORSMiddleware,
    allow_origins=origins,
    allow_credentials=True,
    allow_methods=["GET", "POST", "PUT", "DELETE"],
    allow_headers=["Content-Type", "Authorization", "X-Request-ID"],
)
```

## Regex Origins

```python
app.add_middleware(
    CORSMiddleware,
    allow_origin_regex=r"https://.*\.example\.com",
    allow_methods=["*"],
)
```

## Expose Headers

```python
app.add_middleware(
    CORSMiddleware,
    allow_origins=origins,
    expose_headers=["X-Total-Count", "X-Request-ID"],
)
```

## Preflight Cache

```python
app.add_middleware(
    CORSMiddleware,
    allow_origins=origins,
    max_age=600,  # Cache preflight for 10 minutes
)
```

## C++ Core Integration

When you call `app.run()`, AstraAPI automatically detects `CORSMiddleware` and syncs its configuration to the C++ core:

```python
# In run_server()
for mw in app.user_middleware:
    if mw.cls.__name__ == "CORSMiddleware":
        core_app.configure_cors(
            allow_origins=kw.get("allow_origins", []),
            allow_origin_regex=kw.get("allow_origin_regex"),
            allow_methods=kw.get("allow_methods", ["GET"]),
            allow_headers=kw.get("allow_headers", []),
            allow_credentials=kw.get("allow_credentials", False),
            expose_headers=kw.get("expose_headers", []),
            max_age=kw.get("max_age", 600),
        )
```

This means:
- **CORS preflight** (`OPTIONS` requests) is handled entirely in C++ — no Python invocation
- **CORS headers** on regular responses are added in C++ during response building
- Zero overhead for CORS on the hot path

## Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `allow_origins` | `list[str]` | `[]` | Origins that are permitted. Use `["*"]` for all. |
| `allow_origin_regex` | `str \| None` | `None` | Regex pattern for allowed origins. |
| `allow_methods` | `list[str]` | `["GET"]` | HTTP methods to allow. `["*"]` means all standard methods. |
| `allow_headers` | `list[str]` | `[]` | Headers to allow. `["*"]` means all headers. |
| `allow_credentials` | `bool` | `False` | Allow cookies and auth headers. |
| `expose_headers` | `list[str]` | `[]` | Headers to expose to the browser. |
| `max_age` | `int` | `600` | Preflight cache duration in seconds. |

## Production

```python
import os

origins = os.getenv("ALLOWED_ORIGINS", "").split(",")

app.add_middleware(
    CORSMiddleware,
    allow_origins=[o.strip() for o in origins if o.strip()],
    allow_credentials=True,
    allow_methods=["GET", "POST", "PUT", "DELETE", "PATCH"],
    allow_headers=["Content-Type", "Authorization"],
    max_age=3600,
)
```
