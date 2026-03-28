from astraapi import AstraAPI

app = AstraAPI()


@app.get("/app")
def read_main():
    return {"message": "Hello World from main app"}


subapi = AstraAPI()


@subapi.get("/sub")
def read_sub():
    return {"message": "Hello World from sub API"}


app.mount("/subapi", subapi)
