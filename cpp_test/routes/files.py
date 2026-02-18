"""File upload/download routes - ALL PROTECTED"""
from fastapi import APIRouter, Depends, UploadFile, File, HTTPException
from fastapi.responses import FileResponse
from typing import List
from models import FileResponse as FileResp
from auth import get_current_user
from database import get_db
from datetime import datetime
import os

# Create router without default dependencies
router = APIRouter()

UPLOAD_DIR = "./uploads"
os.makedirs(UPLOAD_DIR, exist_ok=True)

@router.post("/upload", response_model=FileResp)
async def upload_file(file: UploadFile = File(...), current_user: dict = Depends(get_current_user)):
    """Protected: Upload file"""
    filepath = os.path.join(UPLOAD_DIR, file.filename)
    
    with open(filepath, "wb") as f:
        content = await file.read()
        f.write(content)
    
    db = get_db()
    cursor = await db.execute(
        "INSERT INTO files (filename, filepath, user_id) VALUES (?, ?, ?)",
        (file.filename, filepath, current_user["id"])
    )
    await db.commit()
    file_id = cursor.lastrowid
    
    cursor = await db.execute("SELECT * FROM files WHERE id = ?", (file_id,))
    row = await cursor.fetchone()
    
    return FileResp(
        id=row[0],
        filename=row[1],
        filepath=row[2],
        user_id=row[3],
        created_at=datetime.fromisoformat(row[4])
    )

@router.get("/", response_model=List[FileResp])
async def list_files(current_user: dict = Depends(get_current_user)):
    """Protected: List user's files"""
    db = get_db()
    cursor = await db.execute(
        "SELECT * FROM files WHERE user_id = ?",
        (current_user["id"],)
    )
    rows = await cursor.fetchall()
    
    return [
        FileResp(
            id=row[0],
            filename=row[1],
            filepath=row[2],
            user_id=row[3],
            created_at=datetime.fromisoformat(row[4])
        )
        for row in rows
    ]

@router.get("/{file_id}")
async def download_file(file_id: int, current_user: dict = Depends(get_current_user)):
    """Protected: Download file"""
    db = get_db()
    cursor = await db.execute(
        "SELECT * FROM files WHERE id = ? AND user_id = ?",
        (file_id, current_user["id"])
    )
    row = await cursor.fetchone()
    
    if not row:
        raise HTTPException(status_code=404, detail="File not found")
    
    return FileResponse(row[2], filename=row[1])
