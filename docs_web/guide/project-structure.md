# Project Structure

A typical AstraAPI project follows the same patterns as FastAPI, with a few optional conventions for performance and organization.

## Minimal Project

```
myapp/
├── main.py
└── requirements.txt
```

`main.py`:
```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def root():
    return {"ok": True}

if __name__ == "__main__":
    app.run()
```

## Recommended Structure

```
myapp/
├── app/
│   ├── __init__.py
│   ├── main.py          # App factory and entry point
│   ├── config.py        # Settings and environment
│   ├── routers/
│   │   ├── __init__.py
│   │   ├── items.py
│   │   └── users.py
│   ├── models/
│   │   ├── __init__.py
│   │   ├── item.py
│   │   └── user.py
│   ├── dependencies/
│   │   ├── __init__.py
│   │   └── auth.py
│   ├── services/
│   │   ├── __init__.py
│   │   └── item_service.py
│   └── middleware/
│       ├── __init__.py
│       └── timing.py
├── tests/
│   ├── __init__.py
│   └── test_items.py
├── scripts/
│   └── build_core.sh    # Optional: custom C++ build
├── pyproject.toml
└── requirements.txt
```

## The App Factory Pattern

```python
# app/main.py
from astraapi import AstraAPI
from app.routers import items, users
from app.middleware import timing

def create_app() -> AstraAPI:
    app = AstraAPI(
        title="MyApp",
        version="1.0.0",
    )
    
    app.add_middleware(timing.TimingMiddleware)
    app.include_router(items.router)
    app.include_router(users.router)
    
    return app

app = create_app()
```

```python
# app/routers/items.py
from astraapi import APIRouter
from app.models.item import Item

router = APIRouter(prefix="/items", tags=["items"])

@router.get("/")
def list_items() -> list[Item]:
    return []
```

## Configuration with Pydantic Settings

```python
# app/config.py
from pydantic_settings import BaseSettings

class Settings(BaseSettings):
    app_name: str = "MyApp"
    debug: bool = False
    database_url: str = "sqlite:///./app.db"
    
    class Config:
        env_file = ".env"

settings = Settings()
```

## Testing Layout

```python
# tests/test_items.py
from app.main import create_app
from astraapi.testclient import TestClient

app = create_app()
client = TestClient(app)

def test_list_items():
    response = client.get("/items/")
    assert response.status_code == 200
    assert response.json() == []
```

## C++ Core Development

If you are contributing to or customizing the C++ core:

```
cpp_core/
├── CMakeLists.txt
├── include/           # Public headers
│   └── astraapi/
├── src/               # Implementation
│   ├── app.cpp
│   ├── router.cpp
│   ├── json_writer.cpp
│   └── module.cpp
└── third_party/       # llhttp, yyjson, ryu
```

Build with:
```bash
bash scripts/build_core.sh
```

This compiles the C++ core and copies `_astraapi_core.so` to both `astraapi/` and your virtual environment.
