# JSON & HTML Responses

AstraAPI automatically converts return values to JSON responses. For other response types, use response classes.

## Automatic JSON Response

```python
from astraapi import AstraAPI

app = AstraAPI()

@app.get("/items/{item_id}")
def read_item(item_id: int):
    return {"item_id": item_id, "name": "Foo"}
```

AstraAPI detects the `dict` return type and serializes it to JSON using the C++ rapidjson serializer.

## Custom Status Code

```python
from astraapi import status

@app.post("/items/", status_code=status.HTTP_201_CREATED)
def create_item():
    return {"created": True}
```

## Return a Response Directly

```python
from astraapi import Response

@app.get("/legacy/")
def get_legacy():
    data = """<?xml version="1.0"?>
    <shampoo>
        <name>Shiny</name>
    </shampoo>"""
    return Response(content=data, media_type="application/xml")
```

## JSONResponse

```python
from astraapi import JSONResponse

@app.get("/items/")
def read_items():
    return JSONResponse(
        content=[{"id": 1}, {"id": 2}],
        headers={"X-Total-Count": "2"},
    )
```

## HTMLResponse

```python
from astraapi import HTMLResponse

@app.get("/items/", response_class=HTMLResponse)
def read_items():
    return """
    <html>
        <head><title>Items</title></head>
        <body><h1>Hello World</h1></body>
    </html>
    """
```

## PlainTextResponse

```python
from astraapi import PlainTextResponse

@app.get("/", response_class=PlainTextResponse)
def main():
    return "Hello World"
```

## RedirectResponse

```python
from astraapi import RedirectResponse

@app.get("/typer")
def redirect_typer():
    return RedirectResponse("https://typer.tiangolo.com")
```

## Response with Cookies

```python
from astraapi import Response

@app.get("/")
def root():
    response = Response(content="Hello")
    response.set_cookie(key="session", value="abc123")
    return response
```

## Custom Response Class

```python
from astraapi import Response

class MyCustomResponse(Response):
    media_type = "application/x-custom"

@app.get("/", response_class=MyCustomResponse)
def root():
    return "hello"
```

## Response Model

Control the output shape with `response_model`:

```python
from pydantic import BaseModel, EmailStr

class UserOut(BaseModel):
    username: str
    email: EmailStr

class UserIn(BaseModel):
    username: str
    email: EmailStr
    password: str

@app.post("/user/", response_model=UserOut)
def create_user(user: UserIn):
    return user  # Password is automatically excluded!
```

## Exclude Fields

```python
@app.post("/user/", response_model=UserOut, response_model_exclude_unset=True)
def create_user(user: UserIn):
    return {"username": user.username}  # Only returns username
```

## Performance: Response Cache

When an endpoint returns the same `dict` or `list` repeatedly, AstraAPI's C++ response cache kicks in:

```python
@app.get("/config")
def get_config():
    return {"version": "1.0", "debug": False}
```

After the first request, subsequent identical responses are served from cache at **~200ns** instead of **~5μs**.
