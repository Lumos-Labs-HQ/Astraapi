from astraapi import AstraAPI

app = AstraAPI()


@app.on_event("shutdown")
def shutdown_event():
    with open("log.txt", mode="a") as log:
        log.write("Application shutdown")


@app.get("/items/")
async def read_items():
    return [{"name": "Foo"}]
