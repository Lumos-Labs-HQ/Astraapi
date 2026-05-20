import asyncio

from astraapi import AstraAPI, WebSocket, Header, HTTPException, Depends
from astraapi.middleware.cors import CORSMiddleware
from astraapi.responses import StreamingResponse, EventSourceResponse, ServerSentEvent
from pydantic import BaseModel, Field, EmailStr
from typing import List, Optional



class UserCreate(BaseModel):
    name: str = Field(min_length=3, max_length=20)
    age: int = Field(gt=0, lt=100)
    email: EmailStr
    bio: Optional[str] = None

class UserResponse(BaseModel):
    id: int
    name: str
    age: int
    email: str


class Address(BaseModel):
    city: str
    country: str


class NestedUserCreate(BaseModel):
    name: str = Field(min_length=3, max_length=20)
    age: int = Field(gt=0, lt=100)
    email: EmailStr
    address: Address


app = AstraAPI()
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


async def verify_api_key(token: str = Header()):
    if token != "1234":
        raise HTTPException(
            status_code=401,
            detail="Invalid API key"
        )

    return token


async def get_db():
    return "postgresql://localhost"


async def get_service(db: str = Depends(get_db)):
    return {"db": db, "status": "connected"}


@app.get("/async")
async def async_root():
    return {"message": "Hello World"}


async def generate_sse():
    for i in range(5):
        yield ServerSentEvent(data={"step": i, "message": f"Processing {i}"}, event="progress")
        await asyncio.sleep(1)
    yield ServerSentEvent(data={"done": True}, event="complete")


@app.get("/events")
async def events():
    return EventSourceResponse(generate_sse())


@app.get("/protected")
async def protected_endpoint(token: str = Header()):
    if token != "123":
        raise HTTPException(status_code=401, detail="Unauthorized")
    return {"message": "You are authorized!"}

@app.get("/check")
async def check_token(token: str = Header()):
    return {"token": token}


@app.get("/di")
async def di(key = Depends(verify_api_key)):
    return {"message": "Dependency Injection works!", "token": key}


@app.get("/nested-di")
async def nested_di(service = Depends(get_service)):
    return {"service": service}


@app.post("/users", response_model=UserResponse)
async def create_user(user: UserCreate):
    return UserResponse(id=1, name=user.name, age=user.age, email=user.email)


@app.post("/nested-users", response_model=UserResponse)
async def create_nested_user(user: NestedUserCreate):
    return UserResponse(id=1, name=user.name, age=user.age, email=user.email)



class AddressNS(BaseModel):
    city: str = Field(min_length=2)
    zip_code: str = Field(pattern=r"^\d{6}$")


class ItemNS(BaseModel):
    name: str = Field(min_length=2)
    price: float = Field(gt=0)

class UserCreateNS(BaseModel):
    name: str = Field(min_length=3, max_length=20)
    age: int = Field(gt=0, lt=100)
    email: EmailStr
    address: AddressNS
    items: List[ItemNS]

class UserResponseNS(BaseModel):
    success: bool
    username: str
    total_items: int


@app.post("/users-ns", response_model=UserResponseNS)
async def create_user_ns(user: UserCreateNS):

    return UserResponseNS(
        success=True,
        username=user.name,
        total_items=len(user.items),
    )



async def generate_story():

    story = [
        "Once upon a time, ",
        "there was a developer named Rana. ",
        "He was learning FastAPI streaming. ",
        "Then he built an awesome realtime app 🚀"
        "The end."
        "credits: jack"
        "p.s. this is a demo story, not a real one."
        "Hope you enjoyed it!"
        "The moral of the story is: keep learning and building cool stuff!"
    ]

    for chunk in story:
        yield chunk
        await asyncio.sleep(1)


@app.get("/story")
async def story():
    return StreamingResponse(
        generate_story(),
        media_type="text/plain"
    )

@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(data)
    except Exception:
        pass

app.run(host="127.0.0.1", port=8002, workers=1)