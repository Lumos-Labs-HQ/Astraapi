# CORS

Enable Cross-Origin Resource Sharing (CORS) so browsers can make requests to your API from different origins.

## Basic CORS

```python
from astraapi import AstraAPI
from starlette.middleware.cors import CORSMiddleware

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

> ⚠️ **Warning:** `allow_origins=["*"]` with `allow_credentials=True` is a security risk. Don't use this in production.

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

## Per-Route CORS

For per-route control, implement a custom middleware or use a decorator pattern with dependencies.

## CORS in Production

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

## How CORS Works

1. Browser sends `OPTIONS` preflight request
2. Server responds with allowed origins, methods, headers
3. Browser sends actual request

AstraAPI's C++ core handles `OPTIONS` requests efficiently, returning the appropriate CORS headers without invoking Python.
