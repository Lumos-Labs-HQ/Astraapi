# Test Client

AstraAPI's `TestClient` is fundamentally different from FastAPI's. It starts the **real C++ HTTP server** on an ephemeral port — you are testing the actual code path, not a mocked ASGI transport.

## Basic Test

```python
from astraapi import AstraAPI
from astraapi.testclient import TestClient

app = AstraAPI()

@app.get("/")
def read_root():
    return {"msg": "Hello World"}

client = TestClient(app)

def test_read_root():
    response = client.get("/")
    assert response.status_code == 200
    assert response.json() == {"msg": "Hello World"}
```

## How It Works

Unlike FastAPI's TestClient (which mocks the ASGI transport), AstraAPI's TestClient:

1. Starts the **real C++ HTTP server** on an ephemeral local port
2. Uses `httpx.Client` to make HTTP requests against `http://127.0.0.1:<port>`
3. The server is **shared per app instance** (cached by `id(app)`) to avoid exhausting file descriptors across 400+ test modules

This means your tests exercise the exact same C++ parsing, routing, serialization, and transport code that runs in production.

## Run with pytest

```bash
pytest test_main.py -v
```

## POST with JSON

```python
def test_create_item():
    response = client.post(
        "/items/",
        json={"name": "Foo", "price": 45.2},
    )
    assert response.status_code == 201
    assert response.json()["name"] == "Foo"
```

## Query Parameters

```python
def test_read_items():
    response = client.get("/items/?skip=10&limit=5")
    assert response.status_code == 200
    assert len(response.json()) == 5
```

## Headers

```python
def test_with_headers():
    response = client.get(
        "/items/",
        headers={"X-Token": "secret"},
    )
    assert response.status_code == 200
```

## Cookies

```python
def test_with_cookies():
    client.cookies.set("session", "abc123")
    response = client.get("/profile")
    assert response.status_code == 200
```

## Form Data

```python
def test_login():
    response = client.post(
        "/login/",
        data={"username": "johndoe", "password": "secret"},
    )
    assert response.status_code == 200
```

## File Uploads

```python
def test_upload():
    response = client.post(
        "/upload/",
        files={"file": ("test.txt", b"hello world", "text/plain")},
    )
    assert response.status_code == 200
    assert response.json()["filename"] == "test.txt"
```

## Testing Exceptions

```python
def test_not_found():
    response = client.get("/items/999")
    assert response.status_code == 404
    assert response.json()["detail"] == "Item not found"
```

## Dependency Overrides

```python
async def override_get_db():
    return MockDBSession()

app.dependency_overrides[get_db] = override_get_db

def test_with_mock_db():
    response = client.get("/items/")
    assert response.status_code == 200
```

## Lifecycle Events

```python
def test_startup():
    with client:
        response = client.get("/")
        assert response.status_code == 200
```

Using `with client:` ensures startup and shutdown events fire.

## WebSocket Testing

```python
def test_websocket():
    with client.websocket_connect("/ws") as websocket:
        websocket.send_text("Hello")
        data = websocket.receive_text()
        assert data == "Message text was: Hello"
```

WebSocket tests use `websockets.sync.client` in a background thread. The `WebSocketTestSession` provides:
- `send_text`, `send_bytes`, `send_json`
- `receive_text`, `receive_bytes`, `receive_json`
- `close`

## Fixtures

```python
import pytest

@pytest.fixture
def client():
    return TestClient(app)

def test_root(client):
    response = client.get("/")
    assert response.status_code == 200
```

## raise_server_exceptions

By default, server exceptions are raised in the test:

```python
client = TestClient(app, raise_server_exceptions=True)
```

Set to `False` to get the HTTP error response instead.

## Performance Testing

For load testing, use external tools against a running server:

```bash
python main.py &
wrk -t2 -c100 -d10s http://localhost:8000/
```

The TestClient is designed for functional testing, not load testing.
