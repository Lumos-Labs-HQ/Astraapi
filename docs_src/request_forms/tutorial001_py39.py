from astraapi import AstraAPI, Form

app = AstraAPI()


@app.post("/login/")
async def login(username: str = Form(), password: str = Form()):
    return {"username": username}
