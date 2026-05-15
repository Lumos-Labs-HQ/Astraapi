# Hello World

The simplest possible AstraAPI application.

## Code

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/")
def read_root():
    return {"Hello": "World"}

@app.get("/items/{item_id}")
def read_item(item_id: int, q: str | None = None):
    return {"item_id": item_id, "q": q}

if __name__ == "__main__":
    app.run(port=8000)
```

## Run

```bash
python main.py
```

## Test

```bash
curl http://localhost:8000/
# {"Hello":"World"}

curl "http://localhost:8000/items/42?q=search"
# {"item_id":42,"q":"search"}
```

## Complete File

```python
# main.py
import uvloop
from astraapi import AstraAPI

uvloop.install()

app = AstraAPI(title="Hello World API")

@app.get("/")
def root():
    return {"message": "Hello World"}

@app.get("/health")
def health():
    return {"status": "healthy"}

if __name__ == "__main__":
    app.run(port=8000)
```

## Project Structure

```
hello-world/
├── main.py
└── requirements.txt
```

`requirements.txt`:
```
astraapi
uvloop
```

## Docker

```dockerfile
FROM python:3.14-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY main.py .
CMD ["python", "main.py"]
```
