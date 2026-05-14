# Exception Handling

Handle errors gracefully with custom exception handlers.

## HTTPException

```python
from astraapi import AstraAPI, HTTPException

app = AstraAPI()

items = {"foo": "The Foo Wrestlers"}

@app.get("/items/{item_id}")
def read_item(item_id: str):
    if item_id not in items:
        raise HTTPException(status_code=404, detail="Item not found")
    return {"item": items[item_id]}
```

Response for missing item:
```json
{
  "detail": "Item not found"
}
```

## Custom Headers

```python
raise HTTPException(
    status_code=404,
    detail="Item not found",
    headers={"X-Error": "Not found"},
)
```

## Custom Exception Class

```python
class UnicornException(Exception):
    def __init__(self, name: str):
        self.name = name

@app.get("/unicorns/{name}")
def read_unicorn(name: str):
    if name != "yolo":
        raise UnicornException(name=name)
    return {"unicorn": name}

@app.exception_handler(UnicornException)
def unicorn_exception_handler(request: Request, exc: UnicornException):
    return JSONResponse(
        status_code=418,
        content={"message": f"Oops! {exc.name} did something."},
    )
```

## Override Default Handlers

```python
from starlette.exceptions import HTTPException as StarletteHTTPException

@app.exception_handler(StarletteHTTPException)
def http_exception_handler(request: Request, exc: StarletteHTTPException):
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": exc.detail, "path": request.url.path},
    )
```

## RequestValidationError

```python
from starlette.exceptions import HTTPException as StarletteHTTPException
from astraapi import RequestValidationError

@app.exception_handler(RequestValidationError)
def validation_exception_handler(request: Request, exc: RequestValidationError):
    return JSONResponse(
        status_code=422,
        content={
            "message": "Validation failed",
            "errors": exc.errors(),
        },
    )
```

## Global Exception Handler

```python
import traceback

@app.exception_handler(Exception)
def global_exception_handler(request: Request, exc: Exception):
    return JSONResponse(
        status_code=500,
        content={
            "message": "Internal server error",
            "error": str(exc) if app.debug else "Unknown error",
            "traceback": traceback.format_exc() if app.debug else None,
        },
    )
```

## Exception Handler Order

1. Most specific handler matches first
2. Custom `HTTPException` handlers
3. Default Starlette handlers

Register specific handlers before general ones:

```python
@app.exception_handler(UnicornException)    # Specific
def unicorn_handler(...): ...

@app.exception_handler(Exception)           # Catch-all
def global_handler(...): ...
```

## C++ Core Error Handling

The C++ core handles these errors automatically without involving Python:

| Error | C++ Response |
|-------|-------------|
| Invalid HTTP | 400 Bad Request |
| No route match | 404 Not Found |
| Method not allowed | 405 Method Not Allowed |
| Body too large | 413 Payload Too Large |

Python exception handlers are only invoked for errors that occur inside your endpoint or validation logic.
