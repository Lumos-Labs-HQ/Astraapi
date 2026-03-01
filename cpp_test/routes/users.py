"""User routes - ALL PROTECTED"""
from fastapi import APIRouter, Depends, HTTPException, status
from typing import List
from models import UserResponse, CurrentUser
from auth import get_current_user
from database import get_db
from datetime import datetime

router = APIRouter(dependencies=[Depends(get_current_user)])

@router.get("/", response_model=List[UserResponse])
async def list_users(skip: int = 0, limit: int = 10):
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
async def get_me(current_user: CurrentUser = Depends(get_current_user)):
    # current_user already contains id, email, username from the auth dep —
    # re-querying the DB for the same row is redundant and doubles latency.
    # Fetch only the extra fields (is_active, created_at) that CurrentUser lacks.
    db = get_db()
    cursor = await db.execute(
        "SELECT is_active, created_at FROM users WHERE id = ?", (current_user.id,)
    )
    row = await cursor.fetchone()

    return UserResponse(
        id=current_user.id,
        email=current_user.email,
        username=current_user.username,
        is_active=bool(row[0]) if row else True,
        created_at=datetime.fromisoformat(row[1]) if row else datetime.now(),
    )

@router.get("/{user_id}", response_model=UserResponse)
async def get_user(user_id: int):
    db = get_db()
    cursor = await db.execute("SELECT * FROM users WHERE id = ?", (user_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="User not found")
    
    return UserResponse(
        id=row[0],
        email=row[1],
        username=row[2],
        is_active=bool(row[4]),
        created_at=datetime.fromisoformat(row[5])
    )
