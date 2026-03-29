from astraapi import AstraAPI
from astraapi.responses import RedirectResponse

app = AstraAPI()


@app.get("/astraapi", response_class=RedirectResponse)
async def redirect_astraapi():
    return "https://astraapi.tiangolo.com"
