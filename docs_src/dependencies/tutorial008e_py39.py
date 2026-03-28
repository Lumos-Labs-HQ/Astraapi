from astraapi import Depends, AstraAPI

app = AstraAPI()


def get_username():
    try:
        yield "Rick"
    finally:
        print("Cleanup up before response is sent")


@app.get("/users/me")
def get_user_me(username: str = Depends(get_username, scope="function")):
    return username
