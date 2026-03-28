from dataclasses import dataclass

from astraapi import AstraAPI


@dataclass
class Item:
    name: str
    price: float
    description: str | None = None
    tax: float | None = None


app = AstraAPI()


@app.post("/items/")
async def create_item(item: Item):
    return item
