# Headers & Cookies

Access and validate HTTP headers and cookies with full Pydantic support.

## Header Parameters

```python
from typing import Annotated
from astraapi import Header

@app.get("/items/")
def read_items(
    user_agent: Annotated[str | None, Header()] = None,
):
    return {"User-Agent": user_agent}
```

Header names are automatically converted from HTTP convention (`User-Agent`) to Python convention (`user_agent`).

## Required Headers

```python
@app.get("/items/")
def read_items(
    x_token: Annotated[str, Header()],
):
    return {"X-Token": x_token}
```

Missing `X-Token` header returns **422 Unprocessable Entity**.

## Header Validation

```python
from pydantic import Field

@app.get("/items/")
def read_items(
    x_token: Annotated[str | None, Header(min_length=10)] = None,
):
    return {"X-Token": x_token}
```

## Duplicate Headers

```python
from typing import List

@app.get("/items/")
def read_items(
    x_token: Annotated[List[str] | None, Header()] = None,
):
    return {"X-Token values": x_token}
```

`X-Token: foo` and `X-Token: bar` → `{"X-Token values": ["foo", "bar"]}`

## Request Headers Dict

```python
from astraapi import Request

@app.get("/items/")
def read_items(request: Request):
    return {"headers": dict(request.headers)}
```

## Set Response Headers

```python
from astraapi import Response

@app.get("/headers")
def get_headers():
    content = {"message": "Hello World"}
    headers = {"X-Cat-Dog": "alone in the world"}
    return JSONResponse(content=content, headers=headers)
```

## Cookies

### Read Cookies

```python
from typing import Annotated
from astraapi import Cookie

@app.get("/items/")
def read_items(
    session_id: Annotated[str | None, Cookie()] = None,
):
    return {"session_id": session_id}
```

### Set Cookies

```python
from astraapi import Response

@app.get("/cookie")
def create_cookie():
    response = Response(content="Here's a cookie")
    response.set_cookie(
        key="session_id",
        value="abc123",
        max_age=3600,
        httponly=True,
        secure=True,
        samesite="lax",
    )
    return response
```

### Delete Cookies

```python
@app.get("/logout")
def logout():
    response = Response(content="Logged out")
    response.delete_cookie(key="session_id")
    return response
```

## Custom Header Types

```python
from pydantic import BaseModel

class CommonHeaders(BaseModel):
    host: str
    save_data: bool | None = None
    if_modified_since: str | None = None
    traceparent: str | None = None
    
    class Config:
        populate_by_name = True

@app.get("/items/")
def read_items(headers: Annotated[CommonHeaders, Header()]):
    return headers.model_dump()
```
