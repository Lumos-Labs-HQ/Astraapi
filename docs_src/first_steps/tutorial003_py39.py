from astraapi import AstraAPI

app = AstraAPI()


@app.get("/")
def root():
    return {"message": "Hello World"}
