# Create an App

Creating an AstraAPI application is identical to FastAPI.

## The Basics

```python
from astraapi import AstraAPI

app = AstraAPI()
```

This creates an application instance with C++ HTTP core, OpenAPI schema generation, Pydantic validation, dependency injection, and built-in multi-worker server capability.

## Configuration Options

```python
app = AstraAPI(
    title="My API",
    description="A longer description",
    version="1.0.0",
    docs_url="/docs",
    redoc_url="/redoc",
    openapi_url="/openapi.json",
)
```

## Application Events

### Startup and Shutdown

```python
from contextlib import asynccontextmanager

@asynccontextmanager
async def lifespan(app):
    print("Starting up...")
    yield
    print("Shutting down...")

app = AstraAPI(lifespan=lifespan)
```

Or the decorator style:

```python
@app.on_event("startup")
async def startup():
    print("Server starting")

@app.on_event("shutdown")
async def shutdown():
    print("Server stopping")
```

## Include Routers

```python
from astraapi import APIRouter

router = APIRouter(prefix="/items", tags=["items"])

@router.get("/")
def list_items():
    return []

app.include_router(router)
```

## C++ Core Pre-warm

By default, the C++ core initializes lazily. For consistent benchmark results or production deployments, force immediate initialization:

```python
app._sync_routes_to_core()
```

This is called automatically by `app.run()`, so you rarely need to call it manually.
