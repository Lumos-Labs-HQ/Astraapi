from astraapi import AstraAPI
from astraapi.responses import RedirectResponse

app = AstraAPI()


@app.get("/pydantic", response_class=RedirectResponse, status_code=302)
async def redirect_pydantic():
    return "https://docs.pydantic.dev/"
