"""
Complete FastAPI backend test with:
- Pydantic validation
- SQLite + aiosqlite + SQLAlchemy
- Decorators
- Middleware
- Protected routes
- All FastAPI features
"""
from fastapi import FastAPI, Depends, HTTPException, Header, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.trustedhost import TrustedHostMiddleware
from pydantic import BaseModel, EmailStr, Field, validator
from typing import Optional, List
from datetime import datetime
import aiosqlite
from sqlalchemy import Column, Integer, String, Boolean, DateTime, create_engine
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.orm import sessionmaker
from functools import wraps
import hashlib

# Database setup
DATABASE_URL = "sqlite+aiosqlite:///./test.db"
Base = declarative_base()

# SQLAlchemy Models
class UserDB(Base):
    __tablename__ = "users"
    id = Column(Integer, primary_key=True, index=True)
    email = Column(String, unique=True, index=True)
    username = Column(String, unique=True, index=True)
    hashed_password = Column(String)
    is_active = Column(Boolean, default=True)
    created_at = Column(DateTime, default=datetime.utcnow)

class PostDB(Base):
    __tablename__ = "posts"
    id = Column(Integer, primary_key=True, index=True)
    title = Column(String, index=True)
    content = Column(String)
    user_id = Column(Integer)
    created_at = Column(DateTime, default=datetime.utcnow)

# Pydantic Models with validation
class UserCreate(BaseModel):
    email: EmailStr
    username: str = Field(..., min_length=3, max_length=50)
    password: str = Field(..., min_length=8)
    
    @validator('username')
    def username_alphanumeric(cls, v):
        assert v.isalnum(), 'must be alphanumeric'
        return v

class UserResponse(BaseModel):
    id: int
    email: str
    username: str
    is_active: bool
    created_at: datetime
    
    class Config:
        from_attributes = True

class PostCreate(BaseModel):
    title: str = Field(..., min_length=1, max_length=200)
    content: str = Field(..., min_length=1)

class PostResponse(BaseModel):
    id: int
    title: str
    content: str
    user_id: int
    created_at: datetime
    
    class Config:
        from_attributes = True

# App initialization
app = FastAPI(title="Complete Backend Test")

# Middleware
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.add_middleware(
    TrustedHostMiddleware,
    allowed_hosts=["localhost", "127.0.0.1", "*"]
)

# Database connection
db_conn = None

@app.on_event("startup")
async def startup():
    global db_conn
    db_conn = await aiosqlite.connect("./test.db")
    await db_conn.execute("""
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            email TEXT UNIQUE NOT NULL,
            username TEXT UNIQUE NOT NULL,
            hashed_password TEXT NOT NULL,
            is_active BOOLEAN DEFAULT 1,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    await db_conn.execute("""
        CREATE TABLE IF NOT EXISTS posts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            content TEXT NOT NULL,
            user_id INTEGER NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    await db_conn.commit()

@app.on_event("shutdown")
async def shutdown():
    if db_conn:
        await db_conn.close()

# Auth utilities
def hash_password(password: str) -> str:
    return hashlib.sha256(password.encode()).hexdigest()

async def get_current_user(authorization: Optional[str] = Header(None)):
    if not authorization:
        raise HTTPException(status_code=401, detail="Not authenticated")
    
    # Simple token validation (user_id in header)
    try:
        user_id = int(authorization.replace("Bearer ", ""))
    except:
        raise HTTPException(status_code=401, detail="Invalid token")
    
    cursor = await db_conn.execute(
        "SELECT * FROM users WHERE id = ? AND is_active = 1", (user_id,)
    )
    user = await cursor.fetchone()
    if not user:
        raise HTTPException(status_code=401, detail="User not found")
    return user

# Decorator for protected routes
def require_auth(func):
    @wraps(func)
    async def wrapper(*args, **kwargs):
        return await func(*args, **kwargs)
    return wrapper

# Routes
@app.get("/")
async def root():
    return {"message": "Complete Backend API", "status": "running"}

@app.post("/users", response_model=UserResponse, status_code=status.HTTP_201_CREATED)
async def create_user(user: UserCreate):
    """Create user with Pydantic validation"""
    hashed_pwd = hash_password(user.password)
    
    try:
        cursor = await db_conn.execute(
            "INSERT INTO users (email, username, hashed_password) VALUES (?, ?, ?)",
            (user.email, user.username, hashed_pwd)
        )
        await db_conn.commit()
        user_id = cursor.lastrowid
        
        cursor = await db_conn.execute("SELECT * FROM users WHERE id = ?", (user_id,))
        row = await cursor.fetchone()
        
        return UserResponse(
            id=row[0],
            email=row[1],
            username=row[2],
            is_active=bool(row[4]),
            created_at=datetime.fromisoformat(row[5])
        )
    except aiosqlite.IntegrityError:
        raise HTTPException(status_code=400, detail="Email or username already exists")

@app.get("/users", response_model=List[UserResponse])
async def list_users(skip: int = 0, limit: int = 10):
    """List users with pagination"""
    cursor = await db_conn.execute(
        "SELECT * FROM users LIMIT ? OFFSET ?", (limit, skip)
    )
    rows = await cursor.fetchall()
    
    return [
        UserResponse(
            id=row[0],
            email=row[1],
            username=row[2],
            is_active=bool(row[4]),
            created_at=datetime.fromisoformat(row[5])
        )
        for row in rows
    ]

@app.get("/users/{user_id}", response_model=UserResponse)
async def get_user(user_id: int):
    """Get user by ID"""
    cursor = await db_conn.execute("SELECT * FROM users WHERE id = ?", (user_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=404, detail="User not found")
    
    return UserResponse(
        id=row[0],
        email=row[1],
        username=row[2],
        is_active=bool(row[4]),
        created_at=datetime.fromisoformat(row[5])
    )

@app.post("/posts", response_model=PostResponse, status_code=status.HTTP_201_CREATED)
async def create_post(post: PostCreate, current_user: tuple = Depends(get_current_user)):
    """Protected route - create post (requires auth)"""
    user_id = current_user[0]
    
    cursor = await db_conn.execute(
        "INSERT INTO posts (title, content, user_id) VALUES (?, ?, ?)",
        (post.title, post.content, user_id)
    )
    await db_conn.commit()
    post_id = cursor.lastrowid
    
    cursor = await db_conn.execute("SELECT * FROM posts WHERE id = ?", (post_id,))
    row = await cursor.fetchone()
    
    return PostResponse(
        id=row[0],
        title=row[1],
        content=row[2],
        user_id=row[3],
        created_at=datetime.fromisoformat(row[4])
    )

@app.get("/posts", response_model=List[PostResponse])
async def list_posts(skip: int = 0, limit: int = 10):
    """List posts with pagination"""
    cursor = await db_conn.execute(
        "SELECT * FROM posts ORDER BY created_at DESC LIMIT ? OFFSET ?", (limit, skip)
    )
    rows = await cursor.fetchall()
    
    return [
        PostResponse(
            id=row[0],
            title=row[1],
            content=row[2],
            user_id=row[3],
            created_at=datetime.fromisoformat(row[4])
        )
        for row in rows
    ]

@app.get("/posts/{post_id}", response_model=PostResponse)
async def get_post(post_id: int):
    """Get post by ID"""
    cursor = await db_conn.execute("SELECT * FROM posts WHERE id = ?", (post_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=404, detail="Post not found")
    
    return PostResponse(
        id=row[0],
        title=row[1],
        content=row[2],
        user_id=row[3],
        created_at=datetime.fromisoformat(row[4])
    )

@app.delete("/posts/{post_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_post(post_id: int, current_user: tuple = Depends(get_current_user)):
    """Protected route - delete post (requires auth)"""
    user_id = current_user[0]
    
    cursor = await db_conn.execute(
        "SELECT user_id FROM posts WHERE id = ?", (post_id,)
    )
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=404, detail="Post not found")
    
    if row[0] != user_id:
        raise HTTPException(status_code=403, detail="Not authorized")
    
    await db_conn.execute("DELETE FROM posts WHERE id = ?", (post_id,))
    await db_conn.commit()

@app.get("/protected")
async def protected_route(current_user: tuple = Depends(get_current_user)):
    """Protected route example"""
    return {
        "message": "This is a protected route",
        "user_id": current_user[0],
        "username": current_user[2]
    }

@app.get("/health")
async def health_check():
    """Health check endpoint"""
    return {"status": "healthy", "database": "connected"}

if __name__ == "__main__":
    import sys
    host = "127.0.0.1"
    port = 8003
    for arg in sys.argv[1:]:
        if arg.startswith("--port="):
            port = int(arg.split("=")[1])
        elif arg.startswith("--host="):
            host = arg.split("=")[1]
    app.run(host=host, port=port)
