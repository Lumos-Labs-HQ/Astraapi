from typing import Annotated

from astraapi import AstraAPI, Header

app = AstraAPI()


@app.get("/items/")
async def read_items(x_token: Annotated[list[str] | None, Header()] = None):
    return {"X-Token values": x_token}
