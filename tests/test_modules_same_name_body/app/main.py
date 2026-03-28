from astraapi import AstraAPI

from . import a, b

app = AstraAPI()

app.include_router(a.router, prefix="/a")
app.include_router(b.router, prefix="/b")
