"""User routes - ALL PROTECTED by router dependency"""
from fastapi import APIRouter, Depends, HTTPException
from typing import List
from models import UserResponse
from auth import get_current_user
from database import get_db
from datetime import datetime

# Protect ALL routes in this router
router = APIRouter(dependencies=[Depends(get_current_user)])

@router.get("/", response_model=List[UserResponse])
async def list_users(skip: int = 0, limit: int = 10):
    """Protected: List all users"""
    db = get_db()
    cursor = await db.execute("SELECT * FROM users LIMIT ? OFFSET ?", (limit, skip))
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

@router.get("/me", response_model=UserResponse)
async def get_me(current_user: dict = Depends(get_current_user)):
    """Protected: Get current user"""
    db = get_db()
    cursor = await db.execute("SELECT * FROM users WHERE id = ?", (current_user["id"],))
    row = await cursor.fetchone()
    
    return UserResponse(
        id=row[0],
        email=row[1],
        username=row[2],
        is_active=bool(row[4]),
        created_at=datetime.fromisoformat(row[5])
    )

@router.get("/{user_id}", response_model=UserResponse)
async def get_user(user_id: int):
    """Protected: Get user by ID"""
    db = get_db()
    cursor = await db.execute("SELECT * FROM users WHERE id = ?", (user_id,))
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
