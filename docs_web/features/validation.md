# Validation

AstraAPI uses Pydantic for automatic request validation. Define your data models and AstraAPI validates incoming data, generates errors, and produces OpenAPI schemas.

## Basic Validation

```python
from pydantic import BaseModel

class Item(BaseModel):
    name: str
    price: float
    in_stock: bool = True

@app.post("/items/")
def create_item(item: Item):
    return item
```

Invalid request:
```bash
curl -X POST "http://localhost:8000/items/" \
  -H "Content-Type: application/json" \
  -d '{"name": "Foo"}'
```

Response (422):
```json
{
  "detail": [
    {
      "loc": ["body", "price"],
      "msg": "field required",
      "type": "value_error.missing"
    }
  ]
}
```

## Field Constraints

```python
from pydantic import Field

class Item(BaseModel):
    name: str = Field(min_length=3, max_length=50)
    price: float = Field(gt=0, le=1000000)
    quantity: int = Field(ge=0, le=1000)
    tags: list[str] = Field(default_factory=list, max_length=10)
```

## Custom Validators

```python
from pydantic import BaseModel, field_validator

class User(BaseModel):
    name: str
    email: str
    
    @field_validator('email')
    @classmethod
    def validate_email(cls, v):
        if '@' not in v:
            raise ValueError('Invalid email address')
        return v.lower()
    
    @field_validator('name')
    @classmethod
    def validate_name(cls, v):
        if not v.strip():
            raise ValueError('Name cannot be empty')
        return v.strip()
```

## Model Validation

```python
from pydantic import BaseModel, model_validator

class Item(BaseModel):
    price: float
    discount: float = 0.0
    
    @model_validator(mode='after')
    def check_discount(self):
        if self.discount > self.price:
            raise ValueError('Discount cannot exceed price')
        return self
```

## Annotated Validators

```python
from typing import Annotated
from pydantic import Field

@app.post("/items/")
def create_item(
    name: Annotated[str, Field(min_length=3)],
    price: Annotated[float, Field(gt=0)],
):
    return {"name": name, "price": price}
```

## Validation with Types

```python
from datetime import datetime
from uuid import UUID

class Order(BaseModel):
    order_id: UUID
    created_at: datetime
    total: float
    items: list[str]
```

AstraAPI automatically converts string UUIDs and ISO datetime strings to proper Python objects.

## Union Types

```python
from typing import Union

class Cat(BaseModel):
    name: str
    meow_volume: float
    
class Dog(BaseModel):
    name: str
    bark_volume: float

class Pet(BaseModel):
    animal: Union[Cat, Dog]
```

## Discriminated Unions

```python
from typing import Literal

class Cat(BaseModel):
    pet_type: Literal["cat"]
    name: str

class Dog(BaseModel):
    pet_type: Literal["dog"]
    name: str

class Pet(BaseModel):
    pet: Annotated[Union[Cat, Dog], Field(discriminator="pet_type")]
```

## Strict Validation

```python
from pydantic import StrictStr, StrictInt

class Item(BaseModel):
    name: StrictStr      # Won't accept numbers
    count: StrictInt     # Won't accept float strings
```

## Validation Error Format

All validation errors follow a consistent format:

```json
{
  "detail": [
    {
      "loc": ["body", "item", "price"],
      "msg": "ensure this value is greater than 0",
      "type": "value_error.number.not_gt",
      "ctx": {"limit_value": 0}
    }
  ]
}
```

## Performance

Pydantic v2 uses pydantic-core (Rust) for validation, making it extremely fast. AstraAPI doesn't interfere with Pydantic — validation happens in Python before/after the C++ core processes the request. For typical models, validation overhead is **< 1μs**.
