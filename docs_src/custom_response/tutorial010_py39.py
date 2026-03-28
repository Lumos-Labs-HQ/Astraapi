from astraapi import AstraAPI
from astraapi.responses import ORJSONResponse

app = AstraAPI(default_response_class=ORJSONResponse)


@app.get("/items/")
async def read_items():
    return [{"item_id": "Foo"}]
