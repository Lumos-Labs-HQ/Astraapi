from typing import Annotated, Union

from astraapi import AstraAPI, Query

app = AstraAPI()


@app.get("/items/")
async def read_items(
    q: Annotated[
        Union[str, None], Query(min_length=3, max_length=50, pattern="^fixedquery$")
    ] = None,
):
    results = {"items": [{"item_id": "Foo"}, {"item_id": "Bar"}]}
    if q:
        results.update({"q": q})
    return results
