"""Main application entry point"""
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from middleware import LoggingMiddleware, RateLimitMiddleware
from routes import users, posts, auth, files
from database import init_db
from contextlib import asynccontextmanager

@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    await init_db()
    yield
    # Shutdown
    from database import close_db
    await close_db()

app = FastAPI(title="Complete C++ Backend Test", lifespan=lifespan, )

# Middleware stack
# app.add_middleware(RateLimitMiddleware, max_requests=100)
# app.add_middleware(LoggingMiddleware)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
app.add_middleware(TrustedHostMiddleware, allowed_hosts=["*"])

# Include routers
app.include_router(auth.router, prefix="/auth", tags=["auth"])
app.include_router(users.router, prefix="/users", tags=["users"])
app.include_router(posts.router, prefix="/posts", tags=["posts"])
app.include_router(files.router, prefix="/files", tags=["files"])

@app.get("/")
async def root():
    return {"message": "C++ Backend raing", "status": "o"}

@app.get("/health")
async def health():
    return {"status": "health"}

if __name__ == "__main__":   
    host = "0.0.0.0"
    port = 8003
    app.run(host=host, port=port, reload=True)
