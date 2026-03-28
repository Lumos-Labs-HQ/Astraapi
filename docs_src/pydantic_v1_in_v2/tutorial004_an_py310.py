from typing import Annotated

from astraapi import AstraAPI
from astraapi.temp_pydantic_v1_params import Body
from pydantic.v1 import BaseModel


class Item(BaseModel):
    name: str
    description: str | None = None
    size: float


app = AstraAPI()


@app.post("/items/")
async def create_item(item: Annotated[Item, Body(embed=True)]) -> Item:
    return item
