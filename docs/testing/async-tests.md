# Async Testing

Test async endpoints with `pytest-asyncio` or the synchronous `TestClient`.

## Using TestClient (Recommended)

`TestClient` handles async automatically:

```python
def test_async_endpoint():
    response = client.get("/async-items/")
    assert response.status_code == 200
```

Under the hood, TestClient runs the async endpoint in an event loop.

## Using pytest-asyncio

For testing async utilities directly:

```python
import pytest
import asyncio

@pytest.mark.asyncio
async def test_async_utility():
    result = await fetch_from_db()
    assert result is not None
```

Install:
```bash
pip install pytest-asyncio
```

## Async Fixtures

```python
import pytest_asyncio

@pytest_asyncio.fixture
async def db_session():
    session = DBSession()
    yield session
    await session.close()

@pytest.mark.asyncio
async def test_with_db(db_session):
    result = await db_session.query(Item).all()
    assert len(result) > 0
```

## Testing with AsyncClient

```python
import httpx
import pytest

@pytest.mark.asyncio
async def test_with_async_client():
    async with httpx.AsyncClient(app=app, base_url="http://test") as ac:
        response = await ac.get("/")
        assert response.status_code == 200
```

## Testing Background Tasks

```python
def test_background_task():
    response = client.post("/send-notification/test@example.com")
    assert response.status_code == 200
    # Background task runs after response in TestClient
```

## Testing WebSockets

```python
def test_websocket():
    with client.websocket_connect("/ws") as ws:
        ws.send_json({"msg": "hello"})
        data = ws.receive_json()
        assert data["echo"]["msg"] == "hello"
```

## Testing SSE

```python
def test_sse():
    with client.stream("GET", "/events") as response:
        for line in response.iter_lines():
            if line.startswith("data: "):
                data = json.loads(line[6:])
                assert "count" in data
                break
```

## Testing Timeouts

```python
def test_timeout():
    response = client.get("/slow", timeout=0.1)
    assert response.status_code == 504
```

## Database Setup for Tests

```python
import pytest

@pytest.fixture(scope="session", autouse=True)
def setup_test_db():
    # Create test database
    create_database("test_db")
    yield
    # Cleanup
    drop_database("test_db")
```

## Isolated Event Loops

```python
@pytest.fixture
def event_loop():
    loop = asyncio.get_event_loop_policy().new_event_loop()
    yield loop
    loop.close()
```

## Testing Concurrent Requests

```python
import asyncio

@pytest.mark.asyncio
async def test_concurrent():
    async with httpx.AsyncClient(app=app) as ac:
        tasks = [ac.get("/items/") for _ in range(100)]
        responses = await asyncio.gather(*tasks)
        assert all(r.status_code == 200 for r in responses)
```
