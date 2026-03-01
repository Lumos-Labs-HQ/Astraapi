"""Post routes"""
from fastapi import APIRouter, Depends, HTTPException, status
from typing import List
from models import PostCreate, PostResponse, CurrentUser
from auth import get_current_user
from database import get_db
from datetime import datetime

router = APIRouter()

@router.post("/", response_model=PostResponse, status_code=status.HTTP_201_CREATED)
async def create_post(post: PostCreate, current_user: CurrentUser = Depends(get_current_user)):
    db = get_db()
    cursor = await db.execute(
        "INSERT INTO posts (title, content, user_id) VALUES (?, ?, ?)",
        (post.title, post.content, current_user.id)
    )
    await db.commit()
    
    cursor = await db.execute("SELECT * FROM posts WHERE id = ?", (cursor.lastrowid,))
    row = await cursor.fetchone()
    
    return PostResponse(
        id=row[0],
        title=row[1],
        content=row[2],
        user_id=row[3],
        created_at=datetime.fromisoformat(row[4])
    )

@router.get("/", response_model=List[PostResponse])
async def list_posts(skip: int = 0, limit: int = 10):
    db = get_db()
    cursor = await db.execute(
        "SELECT * FROM posts ORDER BY created_at DESC LIMIT ? OFFSET ?",
        (limit, skip)
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

@router.get("/{post_id}", response_model=PostResponse)
async def get_post(post_id: int):
    db = get_db()
    cursor = await db.execute("SELECT * FROM posts WHERE id = ?", (post_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Post not found")
    
    return PostResponse(
        id=row[0],
        title=row[1],
        content=row[2],
        user_id=row[3],
        created_at=datetime.fromisoformat(row[4])
    )

@router.delete("/{post_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_post(post_id: int, current_user: CurrentUser = Depends(get_current_user)):
    db = get_db()
    cursor = await db.execute("SELECT user_id FROM posts WHERE id = ?", (post_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Post not found")
    
    if row[0] != current_user.id:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="Not authorized")
    
    await db.execute("DELETE FROM posts WHERE id = ?", (post_id,))
    await db.commit()
