from astraapi import AstraAPI

app = AstraAPI()


@app.get("/items/", operation_id="some_specific_id_you_define")
async def read_items():
    return [{"item_id": "Foo"}]
