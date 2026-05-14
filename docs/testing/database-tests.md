# Database Testing

Test endpoints that interact with databases using dependency overrides and test databases.

## SQLAlchemy Setup

```python
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker, declarative_base

SQLALCHEMY_DATABASE_URL = "sqlite:///./test.db"
engine = create_engine(SQLALCHEMY_DATABASE_URL, connect_args={"check_same_thread": False})
TestingSessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)
Base = declarative_base()

# Create tables
Base.metadata.create_all(bind=engine)

def override_get_db():
    db = TestingSessionLocal()
    try:
        yield db
    finally:
        db.close()

app.dependency_overrides[get_db] = override_get_db
```

## Test with In-Memory SQLite

```python
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

TEST_DATABASE_URL = "sqlite://"
engine = create_engine(
    TEST_DATABASE_URL,
    connect_args={"check_same_thread": False},
    poolclass=StaticPool,
)
```

## Test Data Seeding

```python
@pytest.fixture
def test_db():
    db = TestingSessionLocal()
    
    # Seed data
    db.add(Item(name="Test Item", price=10.0))
    db.commit()
    
    yield db
    
    # Cleanup
    db.query(Item).delete()
    db.commit()
    db.close()

def test_read_item(test_db):
    response = client.get("/items/1")
    assert response.status_code == 200
    assert response.json()["name"] == "Test Item"
```

## Async Database Testing

```python
from sqlalchemy.ext.asyncio import create_async_engine, AsyncSession

async_engine = create_async_engine("sqlite+aiosqlite:///./test.db")

async def override_get_async_db():
    async with AsyncSession(async_engine) as session:
        yield session

app.dependency_overrides[get_async_db] = override_get_async_db
```

## Transaction Rollback

For test isolation, wrap each test in a transaction:

```python
@pytest.fixture
def db_session():
    connection = engine.connect()
    transaction = connection.begin()
    session = TestingSessionLocal(bind=connection)
    
    yield session
    
    session.close()
    transaction.rollback()
    connection.close()
```

## Testing Migrations

```python
def test_migration():
    # Apply migrations
    alembic.config.main(argv=["upgrade", "head"])
    
    # Test schema
    inspector = inspect(engine)
    assert "items" in inspector.get_table_names()
```

## Mocking Database Calls

```python
from unittest.mock import patch, MagicMock

def test_with_mocked_db():
    mock_db = MagicMock()
    mock_db.query.return_value.all.return_value = [
        Item(name="Mock Item", price=5.0)
    ]
    
    app.dependency_overrides[get_db] = lambda: mock_db
    
    response = client.get("/items/")
    assert response.status_code == 200
    assert len(response.json()) == 1
```

## Testing with Docker

```yaml
# docker-compose.test.yml
version: '3'
services:
  test:
    build: .
    depends_on:
      - test-db
    environment:
      DATABASE_URL: postgresql://test:test@test-db/test
  
  test-db:
    image: postgres:15
    environment:
      POSTGRES_USER: test
      POSTGRES_PASSWORD: test
      POSTGRES_DB: test
```

```bash
docker-compose -f docker-compose.test.yml up --abort-on-container-exit
```

## Performance Testing with Database

```python
def test_db_performance():
    import time
    
    start = time.perf_counter()
    for _ in range(100):
        response = client.get("/items/")
        assert response.status_code == 200
    elapsed = time.perf_counter() - start
    
    assert elapsed < 2.0  # Should complete in under 2 seconds
```
