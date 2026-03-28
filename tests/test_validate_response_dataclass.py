from typing import Optional

import pytest
from astraapi import AstraAPI
from astraapi.exceptions import ResponseValidationError
from astraapi.testclient import TestClient
from pydantic.dataclasses import dataclass

app = AstraAPI()


@dataclass
class Item:
    name: str
    price: Optional[float] = None
    owner_ids: Optional[list[int]] = None


@app.get("/items/invalid", response_model=Item)
def get_invalid():
    return {"name": "invalid", "price": "foo"}


@app.get("/items/innerinvalid", response_model=Item)
def get_innerinvalid():
    return {"name": "double invalid", "price": "foo", "owner_ids": ["foo", "bar"]}


@app.get("/items/invalidlist", response_model=list[Item])
def get_invalidlist():
    return [
        {"name": "foo"},
        {"name": "bar", "price": "bar"},
        {"name": "baz", "price": "baz"},
    ]


client = TestClient(app)


def test_invalid():
    with pytest.raises(ResponseValidationError):
        client.get("/items/invalid")


def test_double_invalid():
    with pytest.raises(ResponseValidationError):
        client.get("/items/innerinvalid")


def test_invalid_list():
    with pytest.raises(ResponseValidationError):
        client.get("/items/invalidlist")
