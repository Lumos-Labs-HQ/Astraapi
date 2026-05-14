# OpenAPI & Documentation

AstraAPI automatically generates **OpenAPI 3.1.0** schemas and interactive documentation from your code. It supports `separate_input_output_schemas`, callbacks, webhooks, and custom response definitions.

## Automatic OpenAPI

When you create an app with typed endpoints, OpenAPI is generated automatically:

```python
from astraapi import AstraAPI
from pydantic import BaseModel

app = AstraAPI(title="My API", version="1.0.0")

class Item(BaseModel):
    name: str
    price: float

@app.post("/items/")
def create_item(item: Item):
    return item
```

Access the schema:
- OpenAPI JSON: [http://localhost:8000/openapi.json](http://localhost:8000/openapi.json)
- Swagger UI: [http://localhost:8000/docs](http://localhost:8000/docs)
- ReDoc: [http://localhost:8000/redoc](http://localhost:8000/redoc)

## Custom OpenAPI

```python
from astraapi.openapi.utils import get_openapi

@app.get("/openapi.json")
def custom_openapi():
    if app.openapi_schema:
        return app.openapi_schema
    
    openapi_schema = get_openapi(
        title="Custom API",
        version="2.0.0",
        description="This is a custom OpenAPI schema",
        routes=app.routes,
    )
    app.openapi_schema = openapi_schema
    return app.openapi_schema
```

## Disable Documentation

```python
app = AstraAPI(docs_url=None, redoc_url=None, openapi_url=None)
```

## Custom Swagger UI

```python
app = AstraAPI(
    docs_url="/api/docs",
    redoc_url="/api/redoc",
    openapi_url="/api/openapi.json",
)
```

## Schema Customization

```python
class Item(BaseModel):
    name: str = Field(..., example="Foo")
    price: float = Field(..., gt=0, example=45.2)
    
    class Config:
        json_schema_extra = {
            "examples": [
                {
                    "name": "Foo",
                    "price": 45.2,
                }
            ]
        }
```

## Response Schema

```python
class ItemOut(BaseModel):
    name: str
    price: float

@app.post("/items/", response_model=ItemOut)
def create_item(item: Item):
    return item
```

## Error Responses

```python
@app.post(
    "/items/",
    responses={
        422: {"description": "Validation Error"},
        500: {"description": "Internal Server Error"},
    },
)
def create_item(item: Item):
    return item
```

## Tags and Descriptions

```python
@app.get("/items/", tags=["items"], summary="List items")
def list_items():
    """Get all items in the system."""
    return []
```

## Webhooks

```python
app = AstraAPI(webhook_url="/webhooks")

@app.webhooks.post("/new-subscription")
def new_subscription(body: Subscription):
    """When a new subscription is created."""
    pass
```

## OAuth2 in Docs

```python
app = AstraAPI(
    swagger_ui_init_oauth={
        "clientId": "your-client-id",
        "clientSecret": "your-client-secret",
        "scopes": ["read", "write"],
    },
)
```

## External Documentation

```python
app = AstraAPI(
    docs_url="/docs",
    redoc_url="/redoc",
    swagger_ui_parameters={"syntaxHighlight.theme": "obsidian"},
)
```

## Schema Generation Performance

OpenAPI schema generation happens at startup (or on first request), not per-request. AstraAPI caches the generated schema, so subsequent requests for `/openapi.json` are served instantly.
