from astraapi import AstraAPI
from astraapi.middleware.trustedhost import TrustedHostMiddleware

app = AstraAPI()

app.add_middleware(
    TrustedHostMiddleware, allowed_hosts=["example.com", "*.example.com"]
)


@app.get("/")
async def main():
    return {"message": "Hello World"}
