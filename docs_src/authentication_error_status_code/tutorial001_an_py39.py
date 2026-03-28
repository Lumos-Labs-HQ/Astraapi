from typing import Annotated

from astraapi import Depends, AstraAPI, HTTPException, status
from astraapi.security import HTTPAuthorizationCredentials, HTTPBearer

app = AstraAPI()


class HTTPBearer403(HTTPBearer):
    def make_not_authenticated_error(self) -> HTTPException:
        return HTTPException(
            status_code=status.HTTP_403_FORBIDDEN, detail="Not authenticated"
        )


CredentialsDep = Annotated[HTTPAuthorizationCredentials, Depends(HTTPBearer403())]


@app.get("/me")
def read_me(credentials: CredentialsDep):
    return {"message": "You are authenticated", "token": credentials.credentials}
