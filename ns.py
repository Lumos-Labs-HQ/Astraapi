from fastapi import FastAPI 



app = FastAPI()

@app.get("/")
def root():
    return {"message": "Hello World"}

@app.get("/async")
async def root():
    return {"message": "Hello World"}


app.run(host="127.0.0.1", port=8002, workers=1)