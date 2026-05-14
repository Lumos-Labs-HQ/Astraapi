# Form Data

Receive data submitted from HTML forms using `Form`.

## Basic Form

```python
from typing import Annotated
from astraapi import Form

@app.post("/login/")
def login(
    username: Annotated[str, Form()],
    password: Annotated[str, Form()],
):
    return {"username": username}
```

HTML form:
```html
<form action="/login/" method="post">
  <input name="username" type="text" />
  <input name="password" type="password" />
  <input type="submit" />
</form>
```

## Form Validation

```python
from pydantic import Field

@app.post("/login/")
def login(
    username: Annotated[str, Form(min_length=3)],
    password: Annotated[str, Form(min_length=8)],
):
    return {"username": username}
```

## Form + File

See [File Uploads](./file-uploads) for combining form fields with file uploads.

## Form Model

```python
from pydantic import BaseModel

class LoginForm(BaseModel):
    username: str
    password: str

@app.post("/login/")
def login(form_data: Annotated[LoginForm, Form()]):
    return {"username": form_data.username}
```

## Form Encoding

AstraAPI supports both:
- `application/x-www-form-urlencoded` (default for small forms)
- `multipart/form-data` (required for file uploads)

The parser auto-detects based on `Content-Type`.

## Arrays in Forms

```python
@app.post("/tags/")
def create_tags(
    tags: Annotated[list[str], Form()],
):
    return {"tags": tags}
```

Form data: `tags=python&tags=fastapi` → `{"tags": ["python", "fastapi"]}`
