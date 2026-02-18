"""Database setup and connection"""
import os
import aiosqlite

db_conn = None

def _get_db_path():
    """Get database path, with fallback for WSL compatibility"""
    # Check for environment variable
    db_dir = os.environ.get("DB_DIR")
    
    if db_dir is None:
        # For WSL, /mnt/ paths can have permission issues, so use /tmp
        script_dir = os.path.dirname(os.path.abspath(__file__))
        if script_dir.startswith("/mnt/"):
            db_dir = "/tmp"
            print(f"Using /tmp for database (WSL detected)")
        else:
            db_dir = script_dir
    
    os.makedirs(db_dir, exist_ok=True)
    return os.path.join(db_dir, "cpp_test.db")

async def init_db():
    global db_conn
    
    db_path = _get_db_path()
    print(f"Initializing database at: {db_path}")
    
    db_conn = await aiosqlite.connect(db_path)
    
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
    
    await db_conn.execute("""
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT NOT NULL,
            filepath TEXT NOT NULL,
            user_id INTEGER NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    
    await db_conn.commit()

async def close_db():
    global db_conn
    if db_conn:
        await db_conn.close()

def get_db():
    if db_conn is None:
        raise RuntimeError("Database not initialized")
    return db_conn
