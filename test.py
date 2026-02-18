"""
FastAPI app with C++ core acceleration enabled.
Used for benchmarking against pure Python FastAPI.
Comprehensive test app: all features (CORS, DI, response_model, WebSocket, etc.)
"""

from fastapi import FastAPI

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
    app.run(host=host, port=port)
