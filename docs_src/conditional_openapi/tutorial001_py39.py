from astraapi import AstraAPI
from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    openapi_url: str = "/openapi.json"


settings = Settings()

app = AstraAPI(openapi_url=settings.openapi_url)


@app.get("/")
def root():
    return {"message": "Hello World"}
