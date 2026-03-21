from fastapi._core_bridge import (
    serialize_error_response,
    serialize_error_list,
)
from fastapi._request import Request
from fastapi._response import JSONResponse, Response
from fastapi._status import WS_1008_POLICY_VIOLATION
from fastapi.exceptions import HTTPException, RequestValidationError, WebSocketRequestValidationError
from fastapi.websockets import WebSocket


def _is_body_allowed_for_status_code(status_code):
    """Inline copy to avoid importing fastapi.utils (which triggers pydantic)."""
    if status_code is None:
        return True
    if status_code in {"default", "1XX", "2XX", "3XX", "4XX", "5XX"}:
        return True
    current_status_code = int(status_code)
    return not (current_status_code < 200 or current_status_code in {204, 205, 304})


async def http_exception_handler(request: Request, exc: HTTPException) -> Response:
    headers = getattr(exc, "headers", None)
    if not _is_body_allowed_for_status_code(exc.status_code):
        return Response(status_code=exc.status_code, headers=headers)
    return JSONResponse(
        {"detail": exc.detail}, status_code=exc.status_code, headers=headers
    )


async def request_validation_exception_handler(
    request: Request, exc: RequestValidationError
) -> JSONResponse:
    # Core fast-path: serialize error dicts directly to JSON bytes in one
    # call, skipping jsonable_encoder() + JSONResponse.render() overhead.
    errors = [{k: v for k, v in e.items() if k != "url"} for e in exc.errors()]
    body_bytes = serialize_error_response(errors)
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
