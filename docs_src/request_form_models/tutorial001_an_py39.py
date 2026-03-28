from typing import Annotated

from astraapi import AstraAPI, Form
from pydantic import BaseModel

app = AstraAPI()


class FormData(BaseModel):
    username: str
    password: str


@app.post("/login/")
async def login(data: Annotated[FormData, Form()]):
    return data
