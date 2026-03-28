from astraapi import AstraAPI
from astraapi.staticfiles import StaticFiles

app = AstraAPI()

app.mount("/static", StaticFiles(directory="static"), name="static")
