from typing import Annotated, Union

from astraapi import AstraAPI, Query

app = AstraAPI()


@app.get("/items/")
async def read_items(q: Annotated[Union[list[str], None], Query()] = None):
    query_items = {"q": q}
    return query_items
