# Production Checklist

Before deploying AstraAPI to production, ensure you have covered these essentials.

## Security

- [ ] **HTTPS only** — use Let's Encrypt or a commercial certificate
- [ ] **Secrets management** — never commit .env files
- [ ] **Authentication** — implement OAuth2, JWT, or API keys
- [ ] **Rate limiting** — add middleware to prevent abuse
- [ ] **CORS** — restrict allow_origins to known domains
- [ ] **Security headers** — HSTS, CSP, X-Frame-Options
- [ ] **Dependency scanning** — run safety check regularly

```python
@app.middleware("http")
async def security_headers(request, call_next):
    response = await call_next(request)
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Strict-Transport-Security"] = "max-age=31536000"
    return response
```

## Performance

- [ ] **uvloop present** — already included in dependencies
- [ ] **Multiple workers** — match CPU core count
- [ ] **Kernel tuned** — somaxconn, tcp_tw_reuse
- [ ] **Static files offloaded** — CDN or Nginx
- [ ] **Response cache utilized** — return static dicts

## Monitoring

- [ ] **Health endpoint** — GET /health returns 200
- [ ] **Metrics** — Prometheus, StatsD, or CloudWatch
- [ ] **Logging** — structured JSON logs
- [ ] **Alerting** — P99 latency, error rate thresholds

```python
@app.get("/health")
def health():
    return {"status": "ok", "version": app.version}

@app.middleware("http")
async def logging_middleware(request, call_next):
    import time
    start = time.perf_counter()
    response = await call_next(request)
    duration = time.perf_counter() - start
    
    logger.info({
        "method": request.method,
        "path": request.url.path,
        "status": response.status_code,
        "duration_ms": round(duration * 1000, 2),
    })
    
    return response
```

## Reliability

- [ ] **Graceful shutdown** — handle SIGTERM properly
- [ ] **Database connection pooling** — do not create connections per request
- [ ] **Circuit breakers** — for external service calls
- [ ] **Request timeouts** — prevent hung connections
- [ ] **Resource limits** — CPU, memory, file descriptors

## Configuration

```python
from pydantic_settings import BaseSettings

class Settings(BaseSettings):
    host: str = "127.0.0.1"
    port: int = 8000
    workers: int = 1
    secret_key: str
    database_url: str
    db_pool_size: int = 10
    
    class Config:
        env_file = ".env"

settings = Settings()
```

## Testing in Production

- [ ] **Smoke tests** — verify critical endpoints after deployment
- [ ] **Canary deployment** — route 1% traffic to new version first
- [ ] **Rollback plan** — know how to revert quickly
- [ ] **Load testing** — verify performance before launch

## Example Production Main

```python
import os
import uvloop
from astraapi import AstraAPI
from contextlib import asynccontextmanager

uvloop.install()

@asynccontextmanager
async def lifespan(app: AstraAPI):
    await init_database()
    yield
    await close_database()

app = AstraAPI(lifespan=lifespan)

@app.get("/health")
def health():
    return {"status": "ok"}

if __name__ == "__main__":
    workers = int(os.getenv("WORKERS", os.cpu_count() or 1))
    app.run(
        host="127.0.0.1",
        port=int(os.getenv("PORT", 8000)),
        workers=workers,
        backlog=65535,
    )
```
