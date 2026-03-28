from typing import Callable

from astraapi import Body, AstraAPI, HTTPException, Request, Response
from astraapi.exceptions import RequestValidationError
from astraapi.routing import APIRoute


class ValidationErrorLoggingRoute(APIRoute):
    def get_route_handler(self) -> Callable:
        original_route_handler = super().get_route_handler()

        async def custom_route_handler(request: Request) -> Response:
            try:
                return await original_route_handler(request)
            except RequestValidationError as exc:
                body = await request.body()
                detail = {"errors": exc.errors(), "body": body.decode()}
                raise HTTPException(status_code=422, detail=detail)

        return custom_route_handler


app = AstraAPI()
app.router.route_class = ValidationErrorLoggingRoute


@app.post("/")
async def sum_numbers(numbers: list[int] = Body()):
    return sum(numbers)
