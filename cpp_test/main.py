"""Main application entry point"""
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from routes import users, posts, auth, files
from database import init_db
from contextlib import asynccontextmanager
from models import MessageResponse, HealthResponse
import os

@asynccontextmanager
async def lifespan(app: FastAPI):
    await init_db()
    yield
    from database import close_db
    await close_db()

app = FastAPI(
    title="FastAPI Backend",
    version="1.0.0",
    lifespan=lifespan
)

# Middleware
app.add_middleware(
    CORSMiddleware,
    allow_origins=os.getenv("CORS_ORIGINS", "*").split(","),
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Routers
app.include_router(auth.router, prefix="/auth", tags=["auth"])
app.include_router(users.router, prefix="/users", tags=["users"])
app.include_router(posts.router, prefix="/posts", tags=["posts"])
app.include_router(files.router, prefix="/files", tags=["files"])

@app.get("/", response_model=MessageResponse)
async def root():
    return MessageResponse(message="FastAPI Backend", status="running")

@app.get("/health", response_model=HealthResponse)
def health():
    return HealthResponse()

if __name__ == "__main__":   
    host = "0.0.0.0"
    port = 8001
    app.run(host=host, port=port, reload=True)
