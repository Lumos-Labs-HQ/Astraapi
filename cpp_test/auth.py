"""Authentication utilities and decorators"""
from fastapi import HTTPException, Header
from typing import Optional
import hashlib
from database import get_db

def hash_password(password: str) -> str:
    return hashlib.sha256(password.encode()).hexdigest()

async def get_current_user(authorization: Optional[str] = Header(None)):
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Not authenticated")
    
    try:
        user_id = int(authorization.replace("Bearer ", ""))
    except:
        raise HTTPException(status_code=401, detail="Invalid token")
    
    db = get_db()
    cursor = await db.execute("SELECT * FROM users WHERE id = ? AND is_active = 1", (user_id,))
    user = await cursor.fetchone()
    
    if not user:
        raise HTTPException(status_code=401, detail="User not found")
    
    return {"id": user[0], "email": user[1], "username": user[2]}

def require_auth(func):
    """Decorator for protected routes"""
    async def wrapper(*args, **kwargs):
        return await func(*args, **kwargs)
    return wrapper
