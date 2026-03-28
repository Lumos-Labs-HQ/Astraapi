from astraapi import AstraAPI
from astraapi.responses import RedirectResponse

app = AstraAPI()


@app.get("/teleport")
async def get_teleport() -> RedirectResponse:
    return RedirectResponse(url="https://www.youtube.com/watch?v=dQw4w9WgXcQ")
