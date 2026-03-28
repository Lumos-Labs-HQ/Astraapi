from astraapi import AstraAPI
from astraapi.responses import PlainTextResponse

app = AstraAPI()


@app.get("/", response_class=PlainTextResponse)
async def main():
    return "Hello World"
