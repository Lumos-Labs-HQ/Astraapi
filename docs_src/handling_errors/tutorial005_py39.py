from astraapi import AstraAPI, Request
from astraapi.encoders import jsonable_encoder
from astraapi.exceptions import RequestValidationError
from astraapi.responses import JSONResponse
from pydantic import BaseModel

app = AstraAPI()


@app.exception_handler(RequestValidationError)
async def validation_exception_handler(request: Request, exc: RequestValidationError):
    return JSONResponse(
        status_code=422,
        content=jsonable_encoder({"detail": exc.errors(), "body": exc.body}),
    )


class Item(BaseModel):
    title: str
    size: int


@app.post("/items/")
async def create_item(item: Item):
    return item
