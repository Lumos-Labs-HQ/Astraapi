from typing import Annotated, Union

from astraapi import Cookie, AstraAPI

app = AstraAPI()


@app.get("/items/")
async def read_items(ads_id: Annotated[Union[str, None], Cookie()] = None):
    return {"ads_id": ads_id}
