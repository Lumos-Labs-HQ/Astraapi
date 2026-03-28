from typing import Annotated, Union

from astraapi import AstraAPI, Header

app = AstraAPI()


@app.get("/items/")
async def read_items(x_token: Annotated[Union[list[str], None], Header()] = None):
    return {"X-Token values": x_token}
