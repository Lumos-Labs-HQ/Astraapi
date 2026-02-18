"""Auth routes"""
from fastapi import APIRouter, HTTPException, status
from models import UserCreate, UserResponse
from auth import hash_password
from database import get_db
from datetime import datetime
import aiosqlite

router = APIRouter()

@router.post("/register", response_model=UserResponse, status_code=status.HTTP_201_CREATED)
async def register(user: UserCreate):
    """Register new user with validation"""
    try:
        print(f"[REGISTER] Received: {user}")
        db = get_db()
        print(f"[REGISTER] DB: {db}")
        
        hashed_pwd = hash_password(user.password)
        print(f"[REGISTER] Hashed password")
        
        cursor = await db.execute(
            "INSERT INTO users (email, username, hashed_password) VALUES (?, ?, ?)",
            (user.email, user.username, hashed_pwd)
        )
        await db.commit()
        user_id = cursor.lastrowid
        print(f"[REGISTER] Created user ID: {user_id}")
        
        cursor = await db.execute("SELECT * FROM users WHERE id = ?", (user_id,))
        row = await cursor.fetchone()
        print(f"[REGISTER] Row: {row}")
        
        return UserResponse(
            id=row[0],
            email=row[1],
            username=row[2],
            is_active=bool(row[4]),
            created_at=datetime.fromisoformat(row[5])
        )
    except aiosqlite.IntegrityError as e:
        print(f"[REGISTER ERROR] IntegrityError: {e}")
        raise HTTPException(status_code=400, detail="Email or username already exists")
    except Exception as e:
        print(f"[REGISTER ERROR] {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()
        raise

@router.post("/login")
async def login(email: str, password: str):
    """Login and get token"""
    db = get_db()
    hashed_pwd = hash_password(password)
    
    cursor = await db.execute(
        "SELECT id FROM users WHERE email = ? AND hashed_password = ?",
        (email, hashed_pwd)
    )
    user = await cursor.fetchone()
    
    if not user:
        raise HTTPException(status_code=401, detail="Invalid credentials")
    
    return {"token": str(user[0]), "token_type": "bearer"}
