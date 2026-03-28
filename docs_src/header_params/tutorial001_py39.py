from typing import Union

from astraapi import AstraAPI, Header

app = AstraAPI()


@app.get("/items/")
async def read_items(user_agent: Union[str, None] = Header(default=None)):
    return {"User-Agent": user_agent}
