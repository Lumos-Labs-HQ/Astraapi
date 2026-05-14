# Request Body

Declare request bodies using Pydantic models. AstraAPI reads the body, validates it, and converts it to your model automatically.

## Basic Body

```python
from astraapi import AstraAPI
from pydantic import BaseModel

app = AstraAPI()

class Item(BaseModel):
    name: str
    description: str | None = None
    price: float
    tax: float | None = None

@app.post("/items/")
def create_item(item: Item):
    return item
```

Send a POST with JSON body:

```bash
curl -X POST "http://localhost:8000/items/" \
  -H "Content-Type: application/json" \
  -d '{"name":"Foo","price":45.2}'
```

Response:
```json
{
  "name": "Foo",
  "description": null,
  "price": 45.2,
  "tax": null
}
```

## Multiple Bodies

```python
@app.put("/items/{item_id}")
def update_item(item_id: int, item: Item, user: User):
    return {"item_id": item_id, "item": item, "user": user}
```

Request body must be a nested JSON:
```json
{
  "item": {"name": "Foo", "price": 45.2},
  "user": {"username": "dave"}
}
```

## Body + Path + Query

```python
@app.put("/items/{item_id}")
def update_item(
    item_id: int,
    item: Item,
    q: str | None = None,
):
    return {"item_id": item_id, "q": q, "item": item}
```

- `item_id` → path parameter
- `q` → query parameter
- `item` → body (detected as Pydantic model)

## Singular Values in Body

```python
from astraapi import Body
from typing import Annotated

@app.put("/items/{item_id}")
def update_item(
    item_id: int,
    importance: Annotated[int, Body()],
):
    return {"item_id": item_id, "importance": importance}
```

Request body is just the integer: `5`

## Embedded Single Body

```python
@app.put("/items/{item_id}")
def update_item(
    item_id: int,
    item: Annotated[Item, Body(embed=True)],
):
    return {"item_id": item_id, "item": item}
```

Request body:
```json
{"item": {"name": "Foo", "price": 45.2}}
```

## Field Validation

```python
from pydantic import Field

class Item(BaseModel):
    name: str = Field(min_length=3, max_length=50)
    price: float = Field(gt=0)
    tags: list[str] = Field(default_factory=list)
```

## Nested Models

```python
class Image(BaseModel):
    url: str
    name: str

class Item(BaseModel):
    name: str
    image: Image | None = None

@app.post("/items/")
def create_item(item: Item):
    return item
```

Request:
```json
{
  "name": "Foo",
  "image": {
    "url": "http://example.com/image.png",
    "name": "cover"
  }
}
```

## Deeply Nested Models

AstraAPI handles arbitrarily deep nesting with the same performance — validation is done by Pydantic, which uses Rust-accelerated code in pydantic-core.

## Raw Body

If you need the raw bytes or string:

```python
from astraapi import Request

@app.post("/items/")
def create_item(request: Request):
    body = request.body()
    return {"body_size": len(body)}
```
