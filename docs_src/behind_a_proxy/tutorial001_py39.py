from astraapi import AstraAPI, Request

app = AstraAPI()


@app.get("/app")
def read_main(request: Request):
    return {"message": "Hello World", "root_path": request.scope.get("root_path")}
