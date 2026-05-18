# Streaming Response

Send large or real-time data without buffering the entire response in memory.

## Basic Streaming

```python
from astraapi import StreamingResponse

@app.get("/stream")
def stream_numbers():
    def generate():
        for i in range(10):
            yield f"data: {i}\n\n"
    
    return StreamingResponse(generate(), media_type="text/event-stream")
```

## File Streaming

```python
from pathlib import Path

@app.get("/video/{video_id}")
def stream_video(video_id: str):
    video_path = Path(f"./videos/{video_id}.mp4")
    
    def iterfile():
        with video_path.open("rb") as f:
            while chunk := f.read(8192):
                yield chunk
    
    return StreamingResponse(iterfile(), media_type="video/mp4")
```

## Async Streaming

```python
@app.get("/stream")
async def stream_data():
    async def generate():
        for i in range(10):
            await asyncio.sleep(1)
            yield f"chunk {i}\n"
    
    return StreamingResponse(generate(), media_type="text/plain")
```

## Server-Sent Events (SSE)

AstraAPI provides native `EventSourceResponse` and `ServerSentEvent` classes for structured SSE streaming.

### Using EventSourceResponse

```python
import asyncio
from astraapi.responses import EventSourceResponse, ServerSentEvent

@app.get("/events")
def events():
    async def event_generator():
        for i in range(100):
            await asyncio.sleep(1)
            yield ServerSentEvent(data={"count": i}, event="update")
    
    return EventSourceResponse(event_generator())
```

`EventSourceResponse` automatically sets `media_type="text/event-stream"` and encodes `ServerSentEvent` objects into the correct SSE format. You can also yield plain `dict`, `str`, or `bytes`:

```python
@app.get("/events")
def events():
    async def event_generator():
        yield {"message": "hello"}      # auto-serialized as JSON
        yield "plain text message"       # sent as-is
        yield b"raw bytes"               # sent as-is
    
    return EventSourceResponse(event_generator())
```

### ServerSentEvent Fields

```python
from astraapi.responses import ServerSentEvent

event = ServerSentEvent(
    data={"user": "alice", "action": "login"},
    event="user-action",
    id="42",
    retry=5000,
)
```

| Field | Type | Description |
|-------|------|-------------|
| `data` | `any` | Event payload. `dict` is auto-serialized to JSON. |
| `event` | `str` | Event name (e.g., `"update"`, `"message"`). |
| `id` | `str` | Event ID for client-side `Last-Event-ID`. |
| `retry` | `int` | Reconnection delay in milliseconds. |

### Client-side

```javascript
const eventSource = new EventSource('/events');

// Default message handler
eventSource.onmessage = (event) => {
    console.log(JSON.parse(event.data));
};

// Named event handler
eventSource.addEventListener('update', (event) => {
    console.log('Update:', JSON.parse(event.data));
});
```

### Legacy: Manual SSE with StreamingResponse

If you prefer full control, you can still use `StreamingResponse` directly:

```python
import asyncio
from astraapi import StreamingResponse

@app.get("/events")
def events():
    async def event_generator():
        for i in range(100):
            await asyncio.sleep(1)
            yield f"data: {{'count': {i}}}\n\n"
    
    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
    )
```

## Custom Headers

```python
@app.get("/download/{file_name}")
def download_file(file_name: str):
    def iterfile():
        with open(file_name, "rb") as f:
            yield from f
    
    return StreamingResponse(
        iterfile(),
        media_type="application/octet-stream",
        headers={"Content-Disposition": f"attachment; filename={file_name}"},
    )
```

## Range Requests

```python
@app.get("/audio/{audio_id}")
def stream_audio(audio_id: str, range: str | None = None):
    audio_path = Path(f"./audio/{audio_id}.mp3")
    file_size = audio_path.stat().st_size
    
    if range:
        start, end = parse_range(range, file_size)
        
        def iter_range():
            with audio_path.open("rb") as f:
                f.seek(start)
                remaining = end - start + 1
                while remaining > 0:
                    chunk = f.read(min(8192, remaining))
                    if not chunk:
                        break
                    remaining -= len(chunk)
                    yield chunk
        
        return StreamingResponse(
            iter_range(),
            media_type="audio/mpeg",
            headers={
                "Content-Range": f"bytes {start}-{end}/{file_size}",
                "Accept-Ranges": "bytes",
            },
            status_code=206,
        )
    
    return StreamingResponse(
        open(audio_path, "rb"),
        media_type="audio/mpeg",
    )
```

## Performance Considerations

Streaming responses bypass the C++ response cache because the body isn't known upfront. Serialization happens in Python or your generator code.

For maximum performance with streaming:
- Use larger chunk sizes (8KB-64KB) to reduce generator overhead
- Consider using `asyncio` generators for I/O-bound streaming
- For file serving, AstraAPI's static file handler is faster than manual streaming
