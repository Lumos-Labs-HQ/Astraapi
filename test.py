

import time

# Measure import time
import_start = time.perf_counter()
from fastapi import FastAPI
import_end = time.perf_counter()
print(f"⏱️  Module import time: {(import_end - import_start) * 1000:.2f}ms")

app = FastAPI(title="FastAPI + Core")

@app.get("/")
async def root():
    return {"message": "Hello World"}

if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8002
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    
    # Measure server startup time
    server_start = time.perf_counter()
    print(f"🚀 Starting server at {host}:{port}...")
    app.run(host=host, port=port)
    server_end = time.perf_counter()
    print(f"⏱️  Server run time: {(server_end - server_start) * 1000:.2f}ms")
