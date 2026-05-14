# Quick Start

Build your first AstraAPI app in 60 seconds.

## 1. Create a File

```bash
mkdir myapp && cd myapp
```

Create `main.py`:

```python
from astraapi import AstraAPI
from pydantic import BaseModel

app = AstraAPI()

class Item(BaseModel):
    name: str
    price: float
    in_stock: bool = True

@app.get("/")
def read_root():
    return {"message": "Hello AstraAPI"}

@app.get("/items/{item_id}")
def read_item(item_id: int, q: str | None = None):
    return {"item_id": item_id, "q": q}

@app.post("/items/")
def create_item(item: Item):
    return item

if __name__ == "__main__":
    app.run(port=8000)
```

## 2. Run the Server

```bash
python main.py
```

You will see:

```
C++ HTTP server running on http://127.0.0.1:8000
Press Ctrl+C to stop
```

AstraAPI auto-detects uvloop (Linux/macOS) or winloop (Windows) and uses the best available event loop. No configuration needed.

## 3. Test It

```bash
curl http://localhost:8000/
# {"message":"Hello AstraAPI"}

curl "http://localhost:8000/items/42?q=search"
# {"item_id":42,"q":"search"}

curl -X POST http://localhost:8000/items/ \
  -H "Content-Type: application/json" \
  -d '{"name":"Widget","price":9.99}'
# {"name":"Widget","price":9.99,"in_stock":true}
```

## 4. OpenAPI Docs

Visit `http://localhost:8000/docs` for interactive Swagger UI, or `http://localhost:8000/redoc` for ReDoc.

AstraAPI generates these automatically from your type annotations.

## With Multiple Workers

```python
if __name__ == "__main__":
    app.run(port=8000, workers=4)
```

AstraAPI spawns 4 independent worker processes, each with its own event loop and C++ core. No gunicorn or uvicorn needed.

## What is Next?

- Explore the [Features](../features/routing)
- Read the [Architecture](../architecture/)
- Check [Benchmarks](../performance/benchmarks)
