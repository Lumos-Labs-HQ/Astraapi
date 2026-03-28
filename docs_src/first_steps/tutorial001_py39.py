from astraapi import AstraAPI

app = AstraAPI()


@app.get("/")
async def root():
    return {"message": "Hello World"}
