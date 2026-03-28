from datetime import datetime
from typing import Union

from astraapi import AstraAPI
from astraapi.encoders import jsonable_encoder
from astraapi.responses import JSONResponse
from pydantic import BaseModel


class Item(BaseModel):
    title: str
    timestamp: datetime
    description: Union[str, None] = None


app = AstraAPI()


@app.put("/items/{id}")
def update_item(id: str, item: Item):
    json_compatible_item_data = jsonable_encoder(item)
    return JSONResponse(content=json_compatible_item_data)
