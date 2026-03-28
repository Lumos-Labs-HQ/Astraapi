from astraapi import Depends, AstraAPI
from astraapi.security import OAuth2PasswordBearer

app = AstraAPI()

oauth2_scheme = OAuth2PasswordBearer(tokenUrl="token")


@app.get("/items/")
async def read_items(token: str = Depends(oauth2_scheme)):
    return {"token": token}
