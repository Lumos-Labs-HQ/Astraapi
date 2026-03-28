from astraapi import AstraAPI

app = AstraAPI(docs_url="/documentation", redoc_url=None)


@app.get("/items/")
async def read_items():
    return [{"name": "Foo"}]
