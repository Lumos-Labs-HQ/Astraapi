# GZip Compression

Compress responses automatically to reduce bandwidth.

## Enable GZip

```python
from astraapi import GZipMiddleware

app.add_middleware(GZipMiddleware, minimum_size=1000)
```

Responses larger than 1000 bytes are automatically compressed.

## Custom Minimum Size

```python
app.add_middleware(GZipMiddleware, minimum_size=500)
```

## Compression Level

```python
app.add_middleware(GZipMiddleware, minimum_size=1000, compresslevel=6)
```

AstraAPI's `GZipMiddleware` uses libdeflate when available for 2-3x faster compression compared to standard zlib. For more control, implement custom middleware.

## Exclude from Compression

Responses with `Content-Encoding` already set are not re-compressed. You can also exclude specific paths in custom middleware.

## Custom GZip Middleware

```python
from starlette.middleware.base import BaseHTTPMiddleware
import gzip

class SelectiveGZipMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request, call_next):
        response = await call_next(request)
        
        # Don't compress small responses or images
        if request.url.path.startswith("/static/images/"):
            return response
        
        body = b""
        async for chunk in response.body_iterator:
            body += chunk
        
        if len(body) > 1000:
            compressed = gzip.compress(body, compresslevel=6)
            response.headers["Content-Encoding"] = "gzip"
            response.headers["Content-Length"] = str(len(compressed))
            response.body = compressed
        
        return response

app.add_middleware(SelectiveGZipMiddleware)
```

## Request Body Compression

AstraAPI also supports gzip-compressed request bodies:

```bash
curl -X POST "http://localhost:8000/items/" \
  -H "Content-Type: application/json" \
  -H "Content-Encoding: gzip" \
  --data-binary @payload.json.gz
```

The C++ core detects `Content-Encoding: gzip` and decompresses the body before passing it to Pydantic validation.
