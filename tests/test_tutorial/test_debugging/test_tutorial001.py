import importlib
import sys

import pytest
from astraapi.testclient import TestClient

MOD_NAME = "docs_src.debugging.tutorial001_py39"


@pytest.fixture(name="client")
def get_client():
    mod = importlib.import_module(MOD_NAME)
    client = TestClient(mod.app)
    return client


def test_get_root(client: TestClient):
    response = client.get("/")
    assert response.status_code == 200
    assert response.json() == {"hello world": "ba"}


def test_openapi_schema(client: TestClient):
    response = client.get("/openapi.json")
    assert response.status_code == 200
    assert response.json() == {
        "openapi": "3.1.0",
        "info": {"title": "AstraAPI", "version": "0.1.0"},
        "paths": {
            "/": {
                "get": {
                    "summary": "Root",
                    "operationId": "root__get",
                    "responses": {
                        "200": {
                            "description": "Successful Response",
                            "content": {"application/json": {"schema": {}}},
                        },
                    },
                }
            }
        },
    }
