from typing import Annotated, Any
from unittest.mock import Mock, patch

from astraapi import Depends, AstraAPI
from astraapi.testclient import TestClient

from docs_src.dependencies.tutorial010_py39 import get_db


def test_get_db():
    app = AstraAPI()

    @app.get("/")
    def read_root(c: Annotated[Any, Depends(get_db)]):
        return {"c": str(c)}

    client = TestClient(app)

    dbsession_mock = Mock()

    with patch(
        "docs_src.dependencies.tutorial010_py39.DBSession",
        return_value=dbsession_mock,
        create=True,
    ):
        response = client.get("/")

    assert response.status_code == 200
    assert response.json() == {"c": str(dbsession_mock)}
