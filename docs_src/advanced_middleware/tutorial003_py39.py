from astraapi import AstraAPI
from astraapi.middleware.gzip import GZipMiddleware

app = AstraAPI()

app.add_middleware(GZipMiddleware, minimum_size=1000, compresslevel=5)


@app.get("/")
async def main():
    return "somebigcontent"
