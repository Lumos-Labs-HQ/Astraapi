from astraapi import Cookie, AstraAPI
from pydantic import BaseModel

app = AstraAPI()


class Cookies(BaseModel):
    session_id: str
    fatebook_tracker: str | None = None
    googall_tracker: str | None = None


@app.get("/items/")
async def read_items(cookies: Cookies = Cookie()):
    return cookies
