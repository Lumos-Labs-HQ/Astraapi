from astraapi import AstraAPI
from pydantic import BaseModel


class Item(BaseModel):
    name: str
    description: str | None = None
    price: float
    tax: float | None = None


app = AstraAPI()


@app.post("/items/")
async def create_item(item: Item):
    return item
