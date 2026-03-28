from astraapi import AstraAPI
from astraapi.middleware.httpsredirect import HTTPSRedirectMiddleware

app = AstraAPI()

app.add_middleware(HTTPSRedirectMiddleware)


@app.get("/")
async def main():
    return {"message": "Hello World"}
