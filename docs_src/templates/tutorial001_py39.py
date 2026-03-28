from astraapi import AstraAPI, Request
from astraapi.responses import HTMLResponse
from astraapi.staticfiles import StaticFiles
from astraapi.templating import Jinja2Templates

app = AstraAPI()

app.mount("/static", StaticFiles(directory="static"), name="static")


templates = Jinja2Templates(directory="templates")


@app.get("/items/{id}", response_class=HTMLResponse)
async def read_item(request: Request, id: str):
    return templates.TemplateResponse(
        request=request, name="item.html", context={"id": id}
    )
