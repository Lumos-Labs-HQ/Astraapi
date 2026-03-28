from typing import Union

from astraapi import AstraAPI, Header
from pydantic import BaseModel

app = AstraAPI()


class CommonHeaders(BaseModel):
    host: str
    save_data: bool
    if_modified_since: Union[str, None] = None
    traceparent: Union[str, None] = None
    x_tag: list[str] = []


@app.get("/items/")
async def read_items(headers: CommonHeaders = Header()):
    return headers
