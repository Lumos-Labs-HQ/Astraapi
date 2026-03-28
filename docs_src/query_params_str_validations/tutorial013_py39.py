from astraapi import AstraAPI, Query

app = AstraAPI()


@app.get("/items/")
async def read_items(q: list = Query(default=[])):
    query_items = {"q": q}
    return query_items
