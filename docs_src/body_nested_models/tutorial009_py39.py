from astraapi import AstraAPI

app = AstraAPI()


@app.post("/index-weights/")
async def create_index_weights(weights: dict[int, float]):
    return weights
