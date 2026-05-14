# Path Parameters

Path parameters are declared in the route path using curly braces `{}`. AstraAPI extracts and validates them automatically.

## Basic Path Parameters

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/items/{item_id}")
def read_item(item_id: int):
    return {"item_id": item_id}
```

AstraAPI converts `item_id` to `int` automatically. If the value can't be converted, it returns a **422 Unprocessable Entity** with a detailed error.

## Types Supported

```python
@app.get("/items/{item_id}")
def read_item(item_id: int):           # Integer
    return {"item_id": item_id}

@app.get("/users/{user_id}")
def read_user(user_id: str):           # String (default)
    return {"user_id": user_id}

@app.get("/prices/{price}")
def read_price(price: float):          # Float
    return {"price": price}

@app.get("/flags/{flag}")
def read_flag(flag: bool):             # Boolean
    return {"flag": flag}
```

## Path Validation with Pydantic

```python
from pydantic import Field
from typing import Annotated

@app.get("/items/{item_id}")
def read_item(
    item_id: Annotated[int, Field(gt=0, le=1000)]
):
    return {"item_id": item_id}
```

Now `GET /items/0` returns 422 with:
```json
{
  "detail": [
    {
      "loc": ["path", "item_id"],
      "msg": "ensure this value is greater than 0",
      "type": "value_error.number.not_gt"
    }
  ]
}
```

## Enum Path Parameters

```python
from enum import Enum

class ModelName(str, Enum):
    alexnet = "alexnet"
    resnet = "resnet"
    lenet = "lenet"

@app.get("/models/{model_name}")
def get_model(model_name: ModelName):
    if model_name == ModelName.alexnet:
        return {"model_name": model_name, "message": "Deep Learning FTW!"}
    return {"model_name": model_name, "message": "Have some residuals"}
```

## Path Converters

```python
@app.get("/files/{file_path:path}")
def read_file(file_path: str):
    return {"file_path": file_path}
```

With `:path`, the parameter can contain slashes: `/files/some/folder/file.txt`.

## Multiple Path Parameters

```python
@app.get("/users/{user_id}/items/{item_id}")
def read_user_item(user_id: int, item_id: str):
    return {"user_id": user_id, "item_id": item_id}
```

## Custom Parameter Types

```python
from pydantic import BaseModel

class ItemId:
    def __init__(self, value: str):
        if not value.startswith("item_"):
            raise ValueError("Item ID must start with 'item_'")
        self.value = value

@app.get("/items/{item_id}")
def read_item(item_id: ItemId):
    return {"item_id": item_id.value}
```

## Performance Note

Path parameter extraction happens in the **C++ core**. The router compiles parameterized paths into a fast state machine, so extracting `/{item_id}` adds only **~100ns** to request processing compared to a static route.
