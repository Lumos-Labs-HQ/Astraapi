"""Auth routes"""
from fastapi import APIRouter, HTTPException, status
from models import UserCreate, UserResponse, LoginRequest, TokenResponse
from auth import hash_password, create_access_token
from database import get_db
from datetime import datetime
import aiosqlite

router = APIRouter()

@router.post("/register", response_model=UserResponse, status_code=status.HTTP_201_CREATED)
async def register(user: UserCreate):
    try:
        db = get_db()
        hashed_pwd = hash_password(user.password)
        
        cursor = await db.execute(
            "INSERT INTO users (email, username, hashed_password) VALUES (?, ?, ?)",
            (user.email, user.username, hashed_pwd)
        )
        await db.commit()
        
        cursor = await db.execute("SELECT * FROM users WHERE id = ?", (cursor.lastrowid,))
        row = await cursor.fetchone()
        
        return UserResponse(
            id=row[0],
            email=row[1],
            username=row[2],
            is_active=bool(row[4]),
            created_at=datetime.fromisoformat(row[5])
        )
    except aiosqlite.IntegrityError:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Email or username already exists"
        )

@router.post("/login", response_model=TokenResponse)
async def login(credentials: LoginRequest):
    db = get_db()
    hashed_pwd = hash_password(credentials.password)
    
    cursor = await db.execute(
        "SELECT id, email FROM users WHERE email = ? AND hashed_password = ?",
        (credentials.email, hashed_pwd)
    )
    user = await cursor.fetchone()
    
    if not user:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials"
        )
    
    access_token = create_access_token(data={"sub": str(user[0]), "email": user[1]})
    return TokenResponse(access_token=access_token)
