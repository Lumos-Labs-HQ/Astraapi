# Middleware

Middleware processes requests before they reach your endpoints and responses before they're sent to clients.

## Add Middleware

```python
from astraapi import AstraAPI
from starlette.middleware.cors import CORSMiddleware

app = AstraAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
```

## Custom Middleware

```python
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import Response
import time

class TimingMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        start = time.perf_counter()
        response = await call_next(request)
        elapsed = time.perf_counter() - start
        response.headers["X-Response-Time"] = f"{elapsed:.4f}s"
        return response

app.add_middleware(TimingMiddleware)
```

## Middleware with Exceptions

```python
class ErrorHandlingMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        try:
            return await call_next(request)
        except Exception as e:
            return Response(
                content=f"Internal error: {str(e)}",
                status_code=500,
            )

app.add_middleware(ErrorHandlingMiddleware)
```

## Logging Middleware

```python
import logging

logger = logging.getLogger("astraapi.access")

class LoggingMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        start = time.perf_counter()
        response = await call_next(request)
        elapsed = time.perf_counter() - start
        logger.info(
            f"{request.method} {request.url.path} "
            f"{response.status_code} {elapsed*1000:.2f}ms"
        )
        return response

app.add_middleware(LoggingMiddleware)
```

## Authentication Middleware

```python
class AuthMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        token = request.headers.get("Authorization")
        if not token:
            return Response("Unauthorized", status_code=401)
        
        request.state.user = await verify_token(token)
        return await call_next(request)

app.add_middleware(AuthMiddleware)
```

Access in endpoints:
```python
@app.get("/profile")
def profile(request: Request):
    return {"user": request.state.user}
```

## Middleware Order

Middleware is executed in the order it's added. The first middleware added is the **outermost** layer:

```python
app.add_middleware(LoggingMiddleware)     # Runs first on request, last on response
app.add_middleware(AuthMiddleware)        # Runs second
app.add_middleware(TimingMiddleware)      # Runs closest to endpoint
```

## Built-in Middleware

| Middleware | Purpose | File |
|-----------|---------|------|
| `CORSMiddleware` | Cross-Origin Resource Sharing | `_middleware_impl.py` |
| `GZipMiddleware` | Response compression with libdeflate | `_middleware_impl.py` |
| `TrustedHostMiddleware` | Host header validation | `_middleware_impl.py` |
| `HTTPSRedirectMiddleware` | Redirect HTTP to HTTPS | `_middleware_impl.py` |
| `ServerErrorMiddleware` | Catches unhandled 500s | `_middleware_impl.py` |
| `ExceptionMiddleware` | Dispatches exception handlers | `_middleware_impl.py` |
| `BaseHTTPMiddleware` | Base class for custom middleware | `_middleware_impl.py` |
| `WSGIMiddleware` | PEP-3333 WSGI bridge | `_middleware_impl.py` |

```python
from astraapi import CORSMiddleware, GZipMiddleware

app.add_middleware(CORSMiddleware, allow_origins=["*"])
app.add_middleware(GZipMiddleware, minimum_size=1000)
```

## Performance Note

Middleware in AstraAPI runs in Python (after the C++ core hands off). The C++ core handles the HTTP parse, route match, parameter extraction, and response serialization before middleware runs, so middleware doesn't affect parsing speed. For maximum performance:

- Keep middleware logic minimal
- Avoid async I/O in middleware unless necessary
- Use `request.state` to pass data instead of re-parsing

The C++ core handles the HTTP parse and route match **before** middleware runs, so middleware doesn't affect parsing speed.
