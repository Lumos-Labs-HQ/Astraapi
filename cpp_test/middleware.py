"""Custom middleware implementations"""
from fastapi import Request
from starlette.middleware.base import BaseHTTPMiddleware
import time

class LoggingMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        start = time.time()
        response = await call_next(request)
        duration = time.time() - start
        print(f"{request.method} {request.url.path} - {response.status_code} - {duration:.3f}s")
        return response

class RateLimitMiddleware(BaseHTTPMiddleware):
    def __init__(self, app, max_requests: int = 100):
        super().__init__(app)
        self.max_requests = max_requests
        self.requests = {}
    
    async def dispatch(self, request: Request, call_next):
        client = request.client.host
        self.requests[client] = self.requests.get(client, 0) + 1
        
        if self.requests[client] > self.max_requests:
            from fastapi.responses import JSONResponse
            return JSONResponse(
                status_code=429,
                content={"detail": "Rate limit exceeded"}
            )
        
        response = await call_next(request)
        return response
