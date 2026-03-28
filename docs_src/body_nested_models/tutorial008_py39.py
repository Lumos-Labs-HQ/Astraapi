from astraapi import AstraAPI
from pydantic import BaseModel, HttpUrl

app = AstraAPI()


class Image(BaseModel):
    url: HttpUrl
    name: str


@app.post("/images/multiple/")
async def create_multiple_images(images: list[Image]):
    return images
