# File Uploads

Upload files using `UploadFile` and `File`. AstraAPI handles `multipart/form-data` parsing efficiently.

## Basic File Upload

```python
from astraapi import UploadFile, File

@app.post("/uploadfile/")
def create_upload_file(file: UploadFile = File(...)):
    return {"filename": file.filename}
```

## File + Form Data

```python
from typing import Annotated
from astraapi import Form

@app.post("/files/")
def create_file(
    file: UploadFile = File(...),
    description: Annotated[str, Form()] = "",
):
    return {
        "filename": file.filename,
        "description": description,
    }
```

## Multiple Files

```python
from typing import List

@app.post("/uploadfiles/")
def create_upload_files(files: List[UploadFile]):
    return {"filenames": [f.filename for f in files]}
```

## File Content

```python
@app.post("/uploadfile/")
async def create_upload_file(file: UploadFile = File(...)):
    contents = await file.read()
    return {
        "filename": file.filename,
        "size": len(contents),
    }
```

## Save Uploaded File

```python
import shutil
from pathlib import Path

UPLOAD_DIR = Path("./uploads")
UPLOAD_DIR.mkdir(exist_ok=True)

@app.post("/uploadfile/")
async def create_upload_file(file: UploadFile = File(...)):
    file_path = UPLOAD_DIR / file.filename
    with file_path.open("wb") as f:
        shutil.copyfileobj(file.file, f)
    return {"filename": file.filename, "saved_to": str(file_path)}
```

## File Validation

```python
from typing import Annotated
from pydantic import Field

@app.post("/uploadfile/")
def create_upload_file(
    file: Annotated[UploadFile, File(max_length=10_000_000)],
):
    return {"filename": file.filename}
```

## UploadFile Attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `filename` | `str` | Original filename |
| `content_type` | `str` | MIME type |
| `file` | `SpooledTemporaryFile` | File-like object |
| `size` | `int` | File size in bytes |

## Large File Uploads

For very large files, use `UploadFile` in an async endpoint to stream data without loading it entirely into memory:

```python
@app.post("/upload-large/")
async def upload_large(file: UploadFile = File(...)):
    total_size = 0
    while chunk := await file.read(8192):
        total_size += len(chunk)
        # Process chunk (e.g., stream to S3)
    return {"total_size": total_size}
```

## Performance Note

File upload parsing happens in Python (multipart parsing is complex), but AstraAPI's C++ core handles the HTTP framing and passes the body efficiently to the multipart parser. For APIs that primarily serve JSON, file uploads won't affect their performance.
