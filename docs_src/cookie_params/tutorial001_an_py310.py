from typing import Annotated

from astraapi import Cookie, AstraAPI

app = AstraAPI()


@app.get("/items/")
async def read_items(ads_id: Annotated[str | None, Cookie()] = None):
    return {"ads_id": ads_id}
