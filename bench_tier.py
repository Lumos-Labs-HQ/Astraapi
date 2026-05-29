from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def root():
    return {"message": "hello"}

@app.get("/json")
def json_root():
    return {"status": "ok", "data": {"id": 1, "name": "test"}}

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8000, workers=1)
