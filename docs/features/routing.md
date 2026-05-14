# Basic Routing

Routing in AstraAPI works exactly like FastAPI. Use decorators to bind URL paths to Python functions.

## HTTP Methods

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def read_root():
    return {"message": "Hello World"}

@app.post("/items")
def create_item():
    return {"created": True}

@app.put("/items/{item_id}")
def update_item(item_id: int):
    return {"updated": item_id}

@app.delete("/items/{item_id}")
def delete_item(item_id: int):
    return {"deleted": item_id}

@app.patch("/items/{item_id}")
def patch_item(item_id: int):
    return {"patched": item_id}

@app.head("/items/{item_id}")
def head_item(item_id: int):
    return {}

@app.options("/items")
def options_items():
    return {}
```

## Route Order

Routes are matched in registration order. Static routes should be defined before dynamic ones:

```python
@app.get("/users/me")      # Matched first
def read_current_user():
    return {"user_id": "current"}

@app.get("/users/{user_id}")  # Matched second
def read_user(user_id: str):
    return {"user_id": user_id}
```

## Multiple Methods

```python
@app.api_route("/items", methods=["GET", "POST"])
def handle_items():
    return {"methods": "GET or POST"}
```

## Path Prefix with Routers

```python
from astraapi import APIRouter

router = APIRouter(prefix="/items", tags=["items"])

@router.get("/")
def list_items():
    return []

@router.get("/{item_id}")
def get_item(item_id: int):
    return {"id": item_id}

app.include_router(router)
```

## Router Options

```python
router = APIRouter(
    prefix="/items",
    tags=["items"],
    dependencies=[Depends(get_token_header)],
    responses={404: {"description": "Not found"}},
)
```

## Include Router Options

```python
app.include_router(
    router,
    prefix="/v1",
    tags=["v1"],
    dependencies=[Depends(verify_api_key)],
    responses={418: {"description": "I'm a teapot"}},
)
```

## Route Metadata

```python
@app.get(
    "/items/{item_id}",
    summary="Read an item",
    description="Read an item by its unique identifier",
    response_description="The requested item",
    deprecated=False,
    tags=["items"],
)
def read_item(item_id: int):
    return {"id": item_id}
```

## Docstrings as Descriptions

```python
@app.get("/items/{item_id}")
def read_item(item_id: int):
    """
    Read an item by ID.
    
    - **item_id**: must be a positive integer
    - Returns the full item object
    """
    return {"id": item_id}
```

The docstring is automatically used in OpenAPI docs.

## Response Models

```python
from pydantic import BaseModel

class Item(BaseModel):
    name: str
    price: float

@app.get("/items/{item_id}", response_model=Item)
def read_item(item_id: int):
    return {"name": "Foo", "price": 50.2}
```

The response is validated and filtered to match the model schema.
