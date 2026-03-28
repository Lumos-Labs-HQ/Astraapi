from typing import Union

from astraapi import AstraAPI, Header

app = AstraAPI()


@app.get("/items/")
async def read_items(
    strange_header: Union[str, None] = Header(default=None, convert_underscores=False),
):
    return {"strange_header": strange_header}
