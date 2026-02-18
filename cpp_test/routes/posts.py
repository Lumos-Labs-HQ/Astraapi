"""Post routes - ALL PROTECTED except list"""
from fastapi import APIRouter, Depends, HTTPException, status
from typing import List
from models import PostCreate, PostResponse
from auth import get_current_user
from database import get_db
from datetime import datetime

# Protect ALL routes by default
router = APIRouter(dependencies=[Depends(get_current_user)])

@router.post("/", response_model=PostResponse, status_code=status.HTTP_201_CREATED)
async def create_post(post: PostCreate, current_user: dict = Depends(get_current_user)):
    """Protected: Create new post"""
    try:
        print(f"[CREATE POST] User: {current_user}, Post: {post}")
        db = get_db()
        print(f"[CREATE POST] DB: {db}")
        
        cursor = await db.execute(
            "INSERT INTO posts (title, content, user_id) VALUES (?, ?, ?)",
            (post.title, post.content, current_user["id"])
        )
        await db.commit()
        post_id = cursor.lastrowid
        print(f"[CREATE POST] Created post ID: {post_id}")
        
        cursor = await db.execute("SELECT * FROM posts WHERE id = ?", (post_id,))
        row = await cursor.fetchone()
        print(f"[CREATE POST] Row: {row}")
        
        return PostResponse(
            id=row[0],
            title=row[1],
            content=row[2],
            user_id=row[3],
            created_at=datetime.fromisoformat(row[4])
        )
    except Exception as e:
        print(f"[CREATE POST ERROR] {type(e).__name__}: {e}")
        import traceback
        traceback.print_exc()
        raise

@router.get("/", response_model=List[PostResponse], dependencies=[])
async def list_posts(skip: int = 0, limit: int = 10):
    """Public: List all posts (override protection)"""
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

@router.get("/{post_id}", response_model=PostResponse, dependencies=[])
async def get_post(post_id: int):
    """Public: Get post by ID (override protection)"""
    db = get_db()
    cursor = await db.execute("SELECT * FROM posts WHERE id = ?", (post_id,))
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

@router.delete("/{post_id}", status_code=status.HTTP_204_NO_CONTENT)
async def delete_post(post_id: int, current_user: dict = Depends(get_current_user)):
    """Protected: Delete post"""
    db = get_db()
    cursor = await db.execute("SELECT user_id FROM posts WHERE id = ?", (post_id,))
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=404, detail="Post not found")
    
    if row[0] != current_user["id"]:
        raise HTTPException(status_code=403, detail="Not authorized")
    
    await db.execute("DELETE FROM posts WHERE id = ?", (post_id,))
    await db.commit()
