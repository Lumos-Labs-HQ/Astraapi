from typing import Union

from astraapi import AstraAPI, status
from pydantic import BaseModel

app = AstraAPI()


class Item(BaseModel):
    name: str
    description: Union[str, None] = None
    price: float
    tax: Union[float, None] = None
    tags: set[str] = set()


@app.post("/items/", status_code=status.HTTP_201_CREATED)
async def create_item(item: Item) -> Item:
    return item
