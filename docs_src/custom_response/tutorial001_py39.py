from astraapi import AstraAPI
from astraapi.responses import UJSONResponse

app = AstraAPI()


@app.get("/items/", response_class=UJSONResponse)
async def read_items():
    return [{"item_id": "Foo"}]
