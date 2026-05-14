# Security

AstraAPI includes complete security tools: OAuth2, JWT, API keys, and HTTP Basic auth. All are FastAPI-compatible.

## OAuth2 Password Flow

```python
from astraapi import AstraAPI, Depends, HTTPException, status
from astraapi.security import OAuth2PasswordBearer, OAuth2PasswordRequestForm

app = AstraAPI()

oauth2_scheme = OAuth2PasswordBearer(tokenUrl="token")

fake_users_db = {
    "johndoe": {
        "username": "johndoe",
        "hashed_password": "fakehashedsecret",
    }
}

def fake_hash_password(password: str):
    return f"fakehashed{password}"

@app.post("/token")
def login(form_data: Annotated[OAuth2PasswordRequestForm, Depends()]):
    user_dict = fake_users_db.get(form_data.username)
    if not user_dict:
        raise HTTPException(status_code=400, detail="Incorrect username or password")
    
    hashed_password = fake_hash_password(form_data.password)
    if hashed_password != user_dict["hashed_password"]:
        raise HTTPException(status_code=400, detail="Incorrect username or password")
    
    return {"access_token": form_data.username, "token_type": "bearer"}

@app.get("/users/me")
def read_users_me(token: Annotated[str, Depends(oauth2_scheme)]):
    user = fake_users_db.get(token)
    if not user:
        raise HTTPException(status_code=401, detail="Invalid token")
    return user
```

## OAuth2 with Scopes

```python
from astraapi.security import SecurityScopes

oauth2_scheme = OAuth2PasswordBearer(
    tokenUrl="token",
    scopes={"me": "Read information about the current user.", "items": "Read items."},
)

@app.get("/users/me")
def read_users_me(
    security_scopes: SecurityScopes,
    token: Annotated[str, Depends(oauth2_scheme)],
):
    # Check scopes
    return {"scopes": security_scopes.scope_str}
```

## JWT Tokens

```python
from datetime import datetime, timedelta
from jose import JWTError, jwt

SECRET_KEY = "your-secret-key"
ALGORITHM = "HS256"
ACCESS_TOKEN_EXPIRE_MINUTES = 30

def create_access_token(data: dict, expires_delta: timedelta | None = None):
    to_encode = data.copy()
    expire = datetime.utcnow() + (expires_delta or timedelta(minutes=15))
    to_encode.update({"exp": expire})
    return jwt.encode(to_encode, SECRET_KEY, algorithm=ALGORITHM)

@app.post("/token")
def login(form_data: Annotated[OAuth2PasswordRequestForm, Depends()]):
    access_token = create_access_token(data={"sub": form_data.username})
    return {"access_token": access_token, "token_type": "bearer"}

def get_current_user(token: Annotated[str, Depends(oauth2_scheme)]):
    try:
        payload = jwt.decode(token, SECRET_KEY, algorithms=[ALGORITHM])
        username: str = payload.get("sub")
        if username is None:
            raise HTTPException(status_code=401)
        return username
    except JWTError:
        raise HTTPException(status_code=401)
```

## API Key

```python
from astraapi.security import APIKeyHeader

api_key_header = APIKeyHeader(name="X-API-Key")

API_KEYS = {"secret-key-1", "secret-key-2"}

def verify_api_key(api_key: str = Security(api_key_header)):
    if api_key not in API_KEYS:
        raise HTTPException(status_code=403, detail="Invalid API Key")
    return api_key

@app.get("/protected")
def protected_route(api_key: Annotated[str, Depends(verify_api_key)]):
    return {"message": "Protected data"}
```

## HTTP Basic Auth

```python
from astraapi.security import HTTPBasic, HTTPBasicCredentials

security = HTTPBasic()

@app.get("/users/me")
def read_current_user(credentials: Annotated[HTTPBasicCredentials, Depends(security)]):
    return {"username": credentials.username, "password": credentials.password}
```

## Dependency with Security

```python
class User(BaseModel):
    username: str
    email: str | None = None
    full_name: str | None = None
    disabled: bool | None = None

async def get_current_active_user(
    current_user: Annotated[User, Depends(get_current_user)],
):
    if current_user.disabled:
        raise HTTPException(status_code=400, detail="Inactive user")
    return current_user

@app.get("/users/me")
def read_users_me(current_user: Annotated[User, Depends(get_current_active_user)]):
    return current_user
```

## Security Best Practices

1. **Never commit secrets** — use environment variables
2. **Use HTTPS** — tokens are sent in headers
3. **Short expiration** — access tokens should expire quickly
4. **Rate limiting** — protect login endpoints
5. **Hash passwords** — use bcrypt or argon2, never plain text

## OpenAPI Integration

Security schemes are automatically documented:

```python
app = AstraAPI(
    title="Secure API",
    swagger_ui_init_oauth={
        "clientId": "your-client-id",
        "clientSecret": "your-client-secret",
    },
)
```

Visit `/docs` to see the "Authorize" button for testing authenticated endpoints.
