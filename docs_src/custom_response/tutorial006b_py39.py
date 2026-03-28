from astraapi import AstraAPI
from astraapi.responses import RedirectResponse

app = AstraAPI()


@app.get("/fastapi", response_class=RedirectResponse)
async def redirect_fastapi():
    return "https://fastapi.tiangolo.com"
