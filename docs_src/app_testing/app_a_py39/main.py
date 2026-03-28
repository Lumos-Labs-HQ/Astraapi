from astraapi import AstraAPI

app = AstraAPI()


@app.get("/")
async def read_main():
    return {"msg": "Hello World"}
