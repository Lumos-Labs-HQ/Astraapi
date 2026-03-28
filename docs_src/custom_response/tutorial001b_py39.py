from astraapi import AstraAPI
from astraapi.responses import ORJSONResponse

app = AstraAPI()


@app.get("/items/", response_class=ORJSONResponse)
async def read_items():
    return ORJSONResponse([{"item_id": "Foo"}])
