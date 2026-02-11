from fastapi._core_bridge import (
    serialize_error_response,
    serialize_error_list,
)
from fastapi.exceptions import RequestValidationError, WebSocketRequestValidationError
from fastapi.utils import is_body_allowed_for_status_code
from fastapi.websockets import WebSocket
from starlette.exceptions import HTTPException
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.status import WS_1008_POLICY_VIOLATION


async def http_exception_handler(request: Request, exc: HTTPException) -> Response:
    headers = getattr(exc, "headers", None)
    if not is_body_allowed_for_status_code(exc.status_code):
        return Response(status_code=exc.status_code, headers=headers)
    return JSONResponse(
        {"detail": exc.detail}, status_code=exc.status_code, headers=headers
    )


async def request_validation_exception_handler(
    request: Request, exc: RequestValidationError
) -> JSONResponse:
    # Core fast-path: serialize error dicts directly to JSON bytes in one
    # call, skipping jsonable_encoder() + JSONResponse.render() overhead.
    body_bytes = serialize_error_response(exc.errors())
    return Response(
        content=body_bytes,
        status_code=422,
        media_type="application/json",
    )


async def websocket_request_validation_exception_handler(
    websocket: WebSocket, exc: WebSocketRequestValidationError
) -> None:
    # Core fast-path for WebSocket error serialization
    error_bytes = serialize_error_list(exc.errors())
    await websocket.close(
        code=WS_1008_POLICY_VIOLATION,
        reason=error_bytes.decode("utf-8"),
    )
