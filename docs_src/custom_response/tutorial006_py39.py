from astraapi import AstraAPI
from astraapi.responses import RedirectResponse

app = AstraAPI()


@app.get("/typer")
async def redirect_typer():
    return RedirectResponse("https://typer.tiangolo.com")
