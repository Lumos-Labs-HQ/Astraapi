from astraapi import AstraAPI

app = AstraAPI()


@app.get("/items/", include_in_schema=False)
async def read_items():
    return [{"item_id": "Foo"}]
