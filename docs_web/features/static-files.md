# Static Files

Serve CSS, JavaScript, images, and other static assets.

## Mount Static Files

```python
from astraapi import AstraAPI
from starlette.staticfiles import StaticFiles

app = AstraAPI()

app.mount("/static", StaticFiles(directory="static"), name="static")
```

Now `GET /static/styles.css` serves `static/styles.css`.

## HTML Directory

```python
app.mount("/", StaticFiles(directory="html", html=True), name="html")
```

With `html=True`, `GET /` serves `html/index.html`.

## SPA Support

For Single Page Applications:

```python
from starlette.staticfiles import StaticFiles

class SPAStaticFiles(StaticFiles):
    async def get_response(self, path: str, scope):
        response = await super().get_response(path, scope)
        if response.status_code == 404:
            response = await super().get_response("index.html", scope)
        return response

app.mount("/", SPAStaticFiles(directory="spa_dist", html=True), name="spa")
```

## Custom Static File Handler

```python
from pathlib import Path
from starlette.responses import FileResponse

@app.get("/files/{file_path:path}")
def read_file(file_path: str):
    file = Path(f"uploads/{file_path}")
    if not file.exists():
        raise HTTPException(status_code=404)
    return FileResponse(file)
```

## Cache Control

```python
app.mount(
    "/static",
    StaticFiles(directory="static"),
    name="static",
)

@app.middleware("http")
async def add_cache_headers(request, call_next):
    response = await call_next(request)
    if request.url.path.startswith("/static/"):
        response.headers["Cache-Control"] = "public, max-age=31536000, immutable"
    return response
```

## Media Types

StaticFiles automatically sets `Content-Type` based on file extension. Supported types include:

| Extension | Content-Type |
|-----------|-------------|
| `.js` | `application/javascript` |
| `.css` | `text/css` |
| `.png` | `image/png` |
| `.jpg` | `image/jpeg` |
| `.svg` | `image/svg+xml` |
| `.woff2` | `font/woff2` |

## Performance Note

StaticFiles is handled by Starlette, not the C++ core. For high-traffic static content, consider:
- Using a CDN (CloudFront, Cloudflare)
- Using Nginx as a reverse proxy for static files
- The C++ core doesn't participate in static file serving
