from astraapi import AstraAPI

app = AstraAPI()


@app.get("/items/")
def read_items():
    return ["plumbus", "portal gun"]
