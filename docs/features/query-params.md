# Query Parameters

Query parameters are declared as function arguments that don't appear in the path. AstraAPI extracts them from the URL query string and validates them with Pydantic.

## Basic Query Parameters

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/items/")
def read_items(skip: int = 0, limit: int = 10):
    return {"skip": skip, "limit": limit}
```

`GET /items/?skip=20&limit=30` → `{"skip": 20, "limit": 30}`

## Optional Parameters

```python
@app.get("/items/{item_id}")
def read_item(item_id: str, q: str | None = None):
    if q:
        return {"item_id": item_id, "q": q}
    return {"item_id": item_id}
```

`q` is optional. `GET /items/foo` and `GET /items/foo?q=bar` both work.

## Type Conversion

```python
@app.get("/items/")
def read_items(
    q: str | None = None,
    short: bool = False,
    price: float = 0.0,
):
    return {"q": q, "short": short, "price": price}
```

- `GET /items/?short=true` → `short=True`
- `GET /items/?price=19.99` → `price=19.99`

## Required Parameters

```python
@app.get("/items/")
def read_items(needy: str):
    return {"needy": needy}
```

`GET /items/` without `?needy=...` returns **422 Unprocessable Entity**.

## List Parameters

```python
from typing import List

@app.get("/items/")
def read_items(q: List[str] | None = None):
    return {"q": q}
```

`GET /items/?q=foo&q=bar` → `{"q": ["foo", "bar"]}`

## Validation with Annotated

```python
from typing import Annotated
from pydantic import Field

@app.get("/items/")
def read_items(
    q: Annotated[str | None, Field(min_length=3, max_length=50)] = None,
    page: Annotated[int, Field(ge=1)] = 1,
):
    return {"q": q, "page": page}
```

## Alias Query Parameters

```python
from typing import Annotated
from pydantic import Field

@app.get("/items/")
def read_items(
    q: Annotated[str | None, Field(alias="item-query")] = None,
):
    return {"q": q}
```

`GET /items/?item-query=foobar` → `{"q": "foobar"}`

## Exclude Unset Parameters

Query parameters with defaults are validated and included in the response model. Use `response_model_exclude_unset` to omit them:

```python
from pydantic import BaseModel

class Item(BaseModel):
    name: str
    description: str | None = None
    price: float | None = None

@app.get("/items/{item_id}", response_model=Item, response_model_exclude_unset=True)
def read_item(item_id: str):
    return {"name": "Foo", "price": 50.2}
```

Response: `{"name": "Foo", "price": 50.2}` (no `description` key)

## DeepObject and Other Styles

AstraAPI supports OpenAPI's query parameter styles:

```python
from typing import Annotated
from astraapi import Query

@app.get("/items/")
def read_items(
    filter: Annotated[dict | None, Query(style="deepObject")] = None,
):
    return {"filter": filter}
```

`GET /items/?filter[name]=foo&filter[price]=10` → `{"filter": {"name": "foo", "price": 10}}`
