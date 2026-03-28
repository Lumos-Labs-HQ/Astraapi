from astraapi import Depends, AstraAPI
from astraapi.security import HTTPBasic, HTTPBasicCredentials

app = AstraAPI()

security = HTTPBasic()


@app.get("/users/me")
def read_current_user(credentials: HTTPBasicCredentials = Depends(security)):
    return {"username": credentials.username, "password": credentials.password}
