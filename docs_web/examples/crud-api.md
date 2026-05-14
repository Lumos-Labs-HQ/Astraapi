# CRUD API

A complete CRUD API with in-memory storage. In production, replace the dict with a real database.

## Code

```python
from astraapi import AstraAPI, HTTPException, status
from pydantic import BaseModel

app = AstraAPI(title="CRUD API")

class Item(BaseModel):
    name: str
    description: str | None = None
    price: float
    in_stock: bool = True

class ItemUpdate(BaseModel):
    name: str | None = None
    description: str | None = None
    price: float | None = None
    in_stock: bool | None = None

items_db: dict[int, dict] = {}
item_counter = 0

def get_next_id() -> int:
    global item_counter
    item_counter += 1
    return item_counter

@app.post("/items/", status_code=status.HTTP_201_CREATED)
def create_item(item: Item):
    item_id = get_next_id()
    items_db[item_id] = item.model_dump()
    return {"id": item_id, **items_db[item_id]}

@app.get("/items/")
def list_items(skip: int = 0, limit: int = 10):
    all_items = [{"id": k, **v} for k, v in items_db.items()]
    return all_items[skip : skip + limit]

@app.get("/items/{item_id}")
def get_item(item_id: int):
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    return {"id": item_id, **items_db[item_id]}

@app.put("/items/{item_id}")
def update_item(item_id: int, item: Item):
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    items_db[item_id] = item.model_dump()
    return {"id": item_id, **items_db[item_id]}

@app.patch("/items/{item_id}")
def patch_item(item_id: int, update: ItemUpdate):
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    stored = items_db[item_id]
    update_data = update.model_dump(exclude_unset=True)
    stored.update(update_data)
    return {"id": item_id, **stored}

@app.delete("/items/{item_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_item(item_id: int):
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    del items_db[item_id]
    return None

if __name__ == "__main__":
    app.run(port=8000)
```

## Test It

```bash
# Create
curl -X POST http://localhost:8000/items/ \
  -H "Content-Type: application/json" \
  -d '{"name":"Widget","price":9.99}'

# List
curl http://localhost:8000/items/

# Get one
curl http://localhost:8000/items/1

# Update
curl -X PUT http://localhost:8000/items/1 \
  -H "Content-Type: application/json" \
  -d '{"name":"Super Widget","price":19.99}'

# Partial update
curl -X PATCH http://localhost:8000/items/1 \
  -H "Content-Type: application/json" \
  -d '{"price":14.99}'

# Delete
curl -X DELETE http://localhost:8000/items/1
```

## With Database

Replace the dict with SQLAlchemy:

```python
from sqlalchemy import create_engine, Column, Integer, String, Float, Boolean
from sqlalchemy.orm import sessionmaker, declarative_base

Base = declarative_base()
engine = create_engine("sqlite:///./items.db")
SessionLocal = sessionmaker(bind=engine)

class ItemModel(Base):
    __tablename__ = "items"
    id = Column(Integer, primary_key=True, index=True)
    name = Column(String)
    description = Column(String, nullable=True)
    price = Column(Float)
    in_stock = Column(Boolean, default=True)

Base.metadata.create_all(bind=engine)

def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()

@app.post("/items/")
def create_item(item: Item, db: Session = Depends(get_db)):
    db_item = ItemModel(**item.model_dump())
    db.add(db_item)
    db.commit()
    db.refresh(db_item)
    return db_item
```
