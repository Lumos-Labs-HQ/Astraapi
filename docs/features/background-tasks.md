# Background Tasks

Run code after returning a response — perfect for logging, sending emails, or processing data asynchronously.

## Basic Background Task

```python
from astraapi import AstraAPI, BackgroundTasks

app = AstraAPI()

def send_email(email: str, message: str):
    # This runs in the event loop after the response is sent
    print(f"Sending email to {email}: {message}")

@app.post("/send-notification/{email}")
def send_notification(email: str, tasks: BackgroundTasks):
    tasks.add_task(send_email, email, message="Hello!")
    return {"message": "Notification sent in the background"}
```

## Async Background Tasks

```python
async def write_log(message: str):
    await asyncio.sleep(1)
    print(f"Log: {message}")

@app.post("/log/")
def create_log(tasks: BackgroundTasks):
    tasks.add_task(write_log, "New log entry")
    return {"message": "Log will be written in background"}
```

## Multiple Tasks

```python
@app.post("/heavy/")
def heavy_operation(tasks: BackgroundTasks):
    tasks.add_task(task_one, "data1")
    tasks.add_task(task_two, "data2")
    tasks.add_task(task_three, "data3")
    return {"message": "3 tasks queued"}
```

Tasks run in the order they were added.

## Task with Return Value (not directly)

Background tasks don't return values to the endpoint. If you need results, use Celery or a task queue:

```python
from celery import Celery

celery_app = Celery("tasks", broker="redis://localhost:6379/0")

@celery_app.task
def process_image(image_id: str):
    # Long-running processing
    pass

@app.post("/upload-image/")
def upload_image(image_id: str, tasks: BackgroundTasks):
    tasks.add_task(process_image.delay, image_id)
    return {"message": "Processing started"}
```

## Dependency with Background Tasks

```python
async def get_background_tasks():
    return BackgroundTasks()

@app.post("/items/")
def create_item(
    item: Item,
    tasks: Annotated[BackgroundTasks, Depends(get_background_tasks)],
):
    tasks.add_task(notify_admins, item)
    return item
```

## When to Use Background Tasks vs Celery

| Use Case | BackgroundTasks | Celery/RQ |
|----------|----------------|-----------|
| Send email after signup | ✅ Perfect | ⚠️ Overkill |
| Log API calls | ✅ Perfect | ✅ Good |
| Process uploaded image | ⚠️ Blocks event loop | ✅ Best |
| Generate PDF report | ⚠️ Slow | ✅ Best |
| Data aggregation | ❌ Don't | ✅ Best |

Background tasks run in the **same process and event loop**. If a task is slow, it blocks the server from handling new requests. For CPU-intensive or long-running work, use a proper task queue.

## Implementation Detail

AstraAPI's background tasks use the same mechanism as FastAPI — they're executed by Starlette's `BackgroundTask` infrastructure after the response is sent. The C++ core returns the response immediately, and Python schedules the background tasks on the event loop.
