# Dependency Injection

AstraAPI's dependency injection system is identical to FastAPI. Declare dependencies as functions and inject them into endpoints.

## Basic Dependency

```python
from astraapi import AstraAPI, Depends

app = AstraAPI()

def common_parameters(q: str | None = None, skip: int = 0, limit: int = 100):
    return {"q": q, "skip": skip, "limit": limit}

@app.get("/items/")
def read_items(commons: Annotated[dict, Depends(common_parameters)]):
    return commons

@app.get("/users/")
def read_users(commons: Annotated[dict, Depends(common_parameters)]):
    return commons
```

## Class Dependencies

```python
class CommonQueryParams:
    def __init__(self, q: str | None = None, skip: int = 0, limit: int = 100):
        self.q = q
        self.skip = skip
        self.limit = limit

@app.get("/items/")
def read_items(commons: Annotated[CommonQueryParams, Depends()]):
    return commons
```

## Sub-dependencies

```python
def query_extractor(q: str | None = None):
    return q

def query_or_default(
    q: Annotated[str | None, Depends(query_extractor)],
):
    if not q:
        return "default"
    return q

@app.get("/items/")
def read_items(q: Annotated[str, Depends(query_or_default)]):
    return {"q": q}
```

## Async Dependencies

```python
async def get_db():
    db = DBSession()
    try:
        yield db
    finally:
        db.close()

@app.get("/items/")
def read_items(db: Annotated[DBSession, Depends(get_db)]):
    return db.query(Item).all()
```

## Dependencies with Yield (Cleanup)

```python
async def get_db():
    db = DBSession()
    try:
        yield db
    finally:
        db.close()
```

The `finally` block runs after the endpoint returns, even if an exception occurs.

## Global Dependencies

```python
async def verify_token(x_token: Annotated[str, Header()]):
    if x_token != "fake-super-secret-token":
        raise HTTPException(status_code=400, detail="X-Token header invalid")

app = AstraAPI(dependencies=[Depends(verify_token)])
```

## Router Dependencies

```python
router = APIRouter(
    prefix="/items",
    dependencies=[Depends(verify_token)],
)
```

## Dependency Overrides (Testing)

```python
async def override_get_db():
    return MockDBSession()

app.dependency_overrides[get_db] = override_get_db
```

## Cached Dependencies

```python
from functools import lru_cache

@lru_cache()
def get_settings():
    return Settings()

@app.get("/info")
def get_info(settings: Annotated[Settings, Depends(get_settings)]):
    return {"app_name": settings.app_name}
```

## Dependency Performance

Dependency resolution in AstraAPI is handled by FastAPI-compatible Python code. For most use cases, the overhead is negligible (< 1μs per request). The C++ core doesn't participate in dependency injection — it delegates to Python after route matching.

For hot paths, consider:
- Using `@lru_cache` for settings and configuration
- Avoiding database queries in dependencies unless necessary
- Keeping dependency graphs shallow
