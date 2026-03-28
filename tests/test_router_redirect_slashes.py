from astraapi import APIRouter, AstraAPI
from astraapi.testclient import TestClient


def test_redirect_slashes_enabled():
    app = AstraAPI()
    router = APIRouter()

    @router.get("/hello/")
    def hello_page() -> str:
        return "Hello, World!"

    app.include_router(router)

    client = TestClient(app)

    response = client.get("/hello/", follow_redirects=False)
    assert response.status_code == 200

    response = client.get("/hello", follow_redirects=False)
    assert response.status_code == 307


def test_redirect_slashes_disabled():
    app = AstraAPI(redirect_slashes=False)
    router = APIRouter()

    @router.get("/hello/")
    def hello_page() -> str:
        return "Hello, World!"

    app.include_router(router)

    client = TestClient(app)

    response = client.get("/hello/", follow_redirects=False)
    assert response.status_code == 200

    response = client.get("/hello", follow_redirects=False)
    assert response.status_code == 404
