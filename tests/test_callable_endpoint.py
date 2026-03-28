from functools import partial
from typing import Optional

from astraapi import AstraAPI
from astraapi.testclient import TestClient


def main(some_arg, q: Optional[str] = None):
    return {"some_arg": some_arg, "q": q}


endpoint = partial(main, "foo")

app = AstraAPI()

app.get("/")(endpoint)


client = TestClient(app)


def test_partial():
    response = client.get("/?q=bar")
    data = response.json()
    assert data == {"some_arg": "foo", "q": "bar"}
