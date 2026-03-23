import contextlib
import functools
import inspect
import io
import json
import types
from collections.abc import (
    AsyncIterator,
    Awaitable,
    Collection,
    Coroutine,
    Generator,
    Mapping,
    Sequence,
)
from contextlib import (
    AbstractAsyncContextManager,
    AbstractContextManager,
    AsyncExitStack,
    asynccontextmanager,
)
from enum import Enum, IntEnum
from typing import (
    Annotated,
    Any,
    Callable,
    Optional,
    TypeVar,
    Union,
)

from annotated_doc import Doc
from fastapi import params
from fastapi._compat import (
    ModelField,
    Undefined,
    annotation_is_pydantic_v1,
    lenient_issubclass,
)
from fastapi.datastructures import Default, DefaultPlaceholder
from fastapi.dependencies.models import Dependant
from fastapi.dependencies.utils import (
    _should_embed_body_fields,
    get_body_field,
    get_dependant,
    get_flat_dependant,
    get_parameterless_sub_dependant,
    get_typed_return_annotation,
    solve_dependencies,
)
from fastapi.encoders import jsonable_encoder
from fastapi.exceptions import (
    EndpointContext,
    FastAPIError,
    PydanticV1NotSupportedError,
    RequestValidationError,
    ResponseValidationError,
    WebSocketRequestValidationError,
)
from fastapi.types import DecoratedCallable, IncEx
from fastapi.utils import (
    create_model_field,
    generate_unique_id,
    get_value_or_default,
    is_body_allowed_for_status_code,
)
from fastapi import _routing_base as routing
from fastapi._exception_handler import wrap_app_handling_exceptions
from fastapi._concurrency import is_async_callable, run_in_threadpool
from fastapi.exceptions import HTTPException
from fastapi._request import Request
from fastapi._response import JSONResponse, Response
from fastapi._routing_base import (
    BaseRoute,
    Match,
    compile_path,
    get_name,
)
from fastapi._routing_base import Mount as Mount  # noqa
from fastapi._types import AppType, ASGIApp, Lifespan, Receive, Scope, Send
from fastapi._websocket import WebSocket
from typing_extensions import deprecated

from fastapi._datastructures_impl import FormData, Headers, UploadFile

from fastapi._core_bridge import (
    CoreRouter,
    compute_dependency_order,
    encode_to_json_bytes,
    parse_multipart_body,
    parse_urlencoded_body,
    process_request,
    register_route_params,
)

# Atomic counter for unique route IDs (used by Core param extractor)
import itertools as _itertools
import base64 as _base64

_route_id_counter = _itertools.count(1)
# Maps core_route_id -> APIRoute for scope["route"] injection
_route_id_to_route: dict = {}
# Maps id(endpoint) -> APIRoute for scope["route"] injection in C++ fast path
_endpoint_id_to_route: dict = {}

from fastapi.exceptions import HTTPException as _DepHTTPExc
_DEP_HTTP_EXC_TYPES: tuple = (_DepHTTPExc,)

from fastapi.security.http import (
    HTTPAuthorizationCredentials as _HTTPAuthCreds,
    HTTPBasicCredentials as _HTTPBasicCreds,
)


# Copy of starlette.routing.request_response modified to include the
# dependencies' AsyncExitStack
def request_response(
    func: Callable[[Request], Union[Awaitable[Response], Response]],
) -> ASGIApp:
    """
    Takes a function or coroutine `func(request) -> response`,
    and returns an ASGI application.
    """
    f: Callable[[Request], Awaitable[Response]] = (
        func if is_async_callable(func) else functools.partial(run_in_threadpool, func)  # type:ignore
    )

    async def app(scope: Scope, receive: Receive, send: Send) -> None:
        request = Request(scope, receive, send)

        async def app(scope: Scope, receive: Receive, send: Send) -> None:
            # Starts customization
            response_awaited = False
            async with AsyncExitStack() as request_stack:
                scope["fastapi_inner_astack"] = request_stack
                async with AsyncExitStack() as function_stack:
                    scope["fastapi_function_astack"] = function_stack
                    response = await f(request)
                await response(scope, receive, send)
                # Continues customization
                response_awaited = True
            if not response_awaited:
                raise FastAPIError(
                    "Response not awaited. There's a high chance that the "
                    "application code is raising an exception and a dependency with yield "
                    "has a block with a bare except, or a block with except Exception, "
                    "and is not raising the exception again. Read more about it in the "
                    "docs: https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-with-yield/#dependencies-with-yield-and-except"
                )

        # Same as in Starlette
        await wrap_app_handling_exceptions(app, request)(scope, receive, send)

    return app


# Copy of starlette.routing.websocket_session modified to include the
# dependencies' AsyncExitStack
def websocket_session(
    func: Callable[[WebSocket], Awaitable[None]],
) -> ASGIApp:
    """
    Takes a coroutine `func(session)`, and returns an ASGI application.
    """
    # assert asyncio.iscoroutinefunction(func), "WebSocket endpoints must be async"

    async def app(scope: Scope, receive: Receive, send: Send) -> None:
        session = WebSocket(scope, receive=receive, send=send)

        async def app(scope: Scope, receive: Receive, send: Send) -> None:
            async with AsyncExitStack() as request_stack:
                scope["fastapi_inner_astack"] = request_stack
                async with AsyncExitStack() as function_stack:
                    scope["fastapi_function_astack"] = function_stack
                    await func(session)

        # Same as in Starlette
        await wrap_app_handling_exceptions(app, session)(scope, receive, send)

    return app


_T = TypeVar("_T")


# Vendored from starlette.routing to avoid importing private symbols
class _AsyncLiftContextManager(AbstractAsyncContextManager[_T]):
    """
    Wraps a synchronous context manager to make it async.

    This is vendored from Starlette to avoid importing private symbols.
    """

    def __init__(self, cm: AbstractContextManager[_T]) -> None:
        self._cm = cm

    async def __aenter__(self) -> _T:
        return self._cm.__enter__()

    async def __aexit__(
        self,
        exc_type: Optional[type[BaseException]],
        exc_value: Optional[BaseException],
        traceback: Optional[types.TracebackType],
    ) -> Optional[bool]:
        return self._cm.__exit__(exc_type, exc_value, traceback)


# Vendored from starlette.routing to avoid importing private symbols
def _wrap_gen_lifespan_context(
    lifespan_context: Callable[[Any], Generator[Any, Any, Any]],
) -> Callable[[Any], AbstractAsyncContextManager[Any]]:
    """
    Wrap a generator-based lifespan context into an async context manager.

    This is vendored from Starlette to avoid importing private symbols.
    """
    cmgr = contextlib.contextmanager(lifespan_context)

    @functools.wraps(cmgr)
    def wrapper(app: Any) -> _AsyncLiftContextManager[Any]:
        return _AsyncLiftContextManager(cmgr(app))

    return wrapper


def _merge_lifespan_context(
    original_context: Lifespan[Any], nested_context: Lifespan[Any]
) -> Lifespan[Any]:
    @asynccontextmanager
    async def merged_lifespan(
        app: AppType,
    ) -> AsyncIterator[Optional[Mapping[str, Any]]]:
        async with original_context(app) as maybe_original_state:
            async with nested_context(app) as maybe_nested_state:
                if maybe_nested_state is None and maybe_original_state is None:
                    yield None  # old ASGI compatibility
                else:
                    yield {**(maybe_nested_state or {}), **(maybe_original_state or {})}

    return merged_lifespan  # type: ignore[return-value]


class _DefaultLifespan:
    """
    Default lifespan context manager that runs on_startup and on_shutdown handlers.

    This is a copy of the Starlette _DefaultLifespan class that was removed
    in Starlette. FastAPI keeps it to maintain backward compatibility with
    on_startup and on_shutdown event handlers.

    Ref: https://github.com/Kludex/starlette/pull/3117
    """

    def __init__(self, router: "APIRouter") -> None:
        self._router = router

    async def __aenter__(self) -> None:
        await self._router._startup()

    async def __aexit__(self, *exc_info: object) -> None:
        await self._router._shutdown()

    def __call__(self: _T, app: object) -> _T:
        return self


# Cache for endpoint context to avoid re-extracting on every request
_endpoint_context_cache: dict[int, EndpointContext] = {}


def _extract_endpoint_context(func: Any) -> EndpointContext:
    """Extract endpoint context with caching to avoid repeated file I/O."""
    func_id = id(func)

    if func_id in _endpoint_context_cache:
        return _endpoint_context_cache[func_id]

    try:
        ctx: EndpointContext = {}

        if (source_file := inspect.getsourcefile(func)) is not None:
            ctx["file"] = source_file
        if (line_number := inspect.getsourcelines(func)[1]) is not None:
            ctx["line"] = line_number
        if (func_name := getattr(func, "__name__", None)) is not None:
            ctx["function"] = func_name
    except Exception:
        ctx = EndpointContext()

    _endpoint_context_cache[func_id] = ctx
    return ctx


async def serialize_response(
    *,
    field: Optional[ModelField] = None,
    response_content: Any,
    include: Optional[IncEx] = None,
    exclude: Optional[IncEx] = None,
    by_alias: bool = True,
    exclude_unset: bool = False,
    exclude_defaults: bool = False,
    exclude_none: bool = False,
    is_coroutine: bool = True,
    endpoint_ctx: Optional[EndpointContext] = None,
) -> Any:
    if field:
        if is_coroutine:
            value, errors = field.validate(response_content, {}, loc=("response",))
        else:
            value, errors = await run_in_threadpool(
                field.validate, response_content, {}, loc=("response",)
            )
        if errors:
            ctx = endpoint_ctx or EndpointContext()
            raise ResponseValidationError(
                errors=errors,
                body=response_content,
                endpoint_ctx=ctx,
            )

        return field.serialize(
            value,
            include=include,
            exclude=exclude,
            by_alias=by_alias,
            exclude_unset=exclude_unset,
            exclude_defaults=exclude_defaults,
            exclude_none=exclude_none,
        )

    else:
        return jsonable_encoder(response_content)


async def run_endpoint_function(
    *, dependant: Dependant, values: dict[str, Any], is_coroutine: bool
) -> Any:
    # Only called by get_request_handler. Has been split into its own function to
    # facilitate profiling endpoints, since inner functions are harder to profile.
    assert dependant.call is not None, "dependant.call must be a function"

    if is_coroutine:
        return await dependant.call(**values)
    else:
        return await run_in_threadpool(dependant.call, **values)


def _extract_boundary(content_type: str) -> Optional[str]:
    """Extract the boundary parameter from a multipart Content-Type header."""
    for part in content_type.split(";"):
        part = part.strip()
        if part.lower().startswith("boundary="):
            value = part[9:]
            # Strip surrounding quotes if present
            if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
                value = value[1:-1]
            return value
    return None


def _build_form_data(parts: list[dict[str, Any]]) -> FormData:
    """Build a Starlette FormData from Core-parsed multipart parts."""
    items: list[tuple[str, Any]] = []
    for part in parts:
        name = part["name"]
        data = part["data"]
        filename = part["filename"]
        if filename is not None:
            # File field → wrap in UploadFile
            content_type = part["content_type"] or "application/octet-stream"
            raw_headers = part.get("headers", {})
            header_list = [
                (k.encode("latin-1"), v.encode("latin-1"))
                for k, v in raw_headers.items()
            ]
            upload = UploadFile(
                file=io.BytesIO(data),
                size=len(data),
                filename=filename,
                headers=Headers(raw=header_list),
            )
            items.append((name, upload))
        else:
            # Regular form field → decode as string
            try:
                items.append((name, data.decode("utf-8")))
            except (UnicodeDecodeError, AttributeError):
                items.append((name, data))
    return FormData(items)


def get_request_handler(
    dependant: Dependant,
    body_field: Optional[ModelField] = None,
    status_code: Optional[int] = None,
    response_class: Union[type[Response], DefaultPlaceholder] = Default(JSONResponse),
    response_field: Optional[ModelField] = None,
    response_model_include: Optional[IncEx] = None,
    response_model_exclude: Optional[IncEx] = None,
    response_model_by_alias: bool = True,
    response_model_exclude_unset: bool = False,
    response_model_exclude_defaults: bool = False,
    response_model_exclude_none: bool = False,
    dependency_overrides_provider: Optional[Any] = None,
    embed_body_fields: bool = False,
    core_route_id: Optional[int] = None,
    batch_specs: Optional[list[tuple[str, str, str, str, bool, bool, bool]]] = None,
) -> Callable[[Request], Coroutine[Any, Any, Response]]:
    assert dependant.call is not None, "dependant.call must be a function"
    is_coroutine = dependant.is_coroutine_callable
    is_body_form = body_field and isinstance(body_field.field_info, params.Form)
    if isinstance(response_class, DefaultPlaceholder):
        actual_response_class: type[Response] = response_class.value
    else:
        actual_response_class = response_class

    async def app(request: Request) -> Response:
        response: Union[Response, None] = None
        file_stack = request.scope.get("fastapi_middleware_astack")
        assert isinstance(file_stack, AsyncExitStack), (
            "fastapi_middleware_astack not found in request scope"
        )

        # v1.0: Store Core route ID on scope for solve_dependencies
        if core_route_id is not None:
            request.scope["_core_route_id"] = core_route_id
        # Store pre-computed batch specs on scope for solve_dependencies
        if batch_specs is not None:
            request.scope["_batch_specs"] = batch_specs

        # Extract endpoint context for error messages
        endpoint_ctx = (
            _extract_endpoint_context(dependant.call)
            if dependant.call
            else EndpointContext()
        )

        if dependant.path:
            # For mounted sub-apps, include the mount path prefix
            mount_path = request.scope.get("root_path", "").rstrip("/")
            endpoint_ctx["path"] = f"{request.method} {mount_path}{dependant.path}"

        # Read body and auto-close files
        try:
            body: Any = None
            if body_field:
                if is_body_form:
                    body_bytes_form = await request.body()
                    ct = request.headers.get("content-type", "")
                    if "multipart/form-data" in ct:
                        boundary = _extract_boundary(ct)
                        if boundary and body_bytes_form:
                            parts = parse_multipart_body(
                                body_bytes_form, boundary
                            )
                            body = _build_form_data(parts)
                            file_stack.push_async_callback(
                                body.close
                            )
                        else:
                            body = await request.form()
                            file_stack.push_async_callback(
                                body.close
                            )
                    elif (
                        "application/x-www-form-urlencoded" in ct
                    ):
                        if body_bytes_form:
                            pairs = parse_urlencoded_body(
                                body_bytes_form
                            )
                            body = FormData(pairs)
                        else:
                            body = FormData()
                    else:
                        body = await request.form()
                        file_stack.push_async_callback(
                            body.close
                        )
                else:
                    body_bytes = await request.body()
                    if body_bytes:
                        json_body: Any = Undefined
                        # Single Core call does content-type check
                        # + JSON parse with GIL released. Also pre-parses
                        # query string for use by solve_dependencies later.
                        raw_hdrs = request.scope.get("headers", [])
                        qs = request.scope.get(
                            "query_string", b""
                        ).decode("latin-1")
                        req_result = process_request(
                            query_string=qs,
                            raw_headers=raw_hdrs,
                            body=body_bytes,
                        )
                        if req_result["json_body"] is not None:
                            json_body = req_result["json_body"]
                        # Cache parsed data for solve_dependencies
                        request.scope[
                            "_core_query_params"
                        ] = req_result["query_params"]
                        request.scope[
                            "_core_headers"
                        ] = req_result["headers"]
                        request.scope[
                            "_core_cookies"
                        ] = req_result["cookies"]
                        if json_body != Undefined:
                            body = json_body
                        else:
                            body = body_bytes
        except json.JSONDecodeError as e:
            validation_error = RequestValidationError(
                [
                    {
                        "type": "json_invalid",
                        "loc": ("body", e.pos),
                        "msg": "JSON decode error",
                        "input": {},
                        "ctx": {"error": e.msg},
                    }
                ],
                body=e.doc,
                endpoint_ctx=endpoint_ctx,
            )
            raise validation_error from e
        except HTTPException:
            # If a middleware raises an HTTPException, it should be raised again
            raise
        except Exception as e:
            http_error = HTTPException(
                status_code=400, detail="There was an error parsing the body"
            )
            raise http_error from e

        # Solve dependencies and run path operation function, auto-closing dependencies
        errors: list[Any] = []
        async_exit_stack = request.scope.get("fastapi_inner_astack")
        assert isinstance(async_exit_stack, AsyncExitStack), (
            "fastapi_inner_astack not found in request scope"
        )
        solved_result = await solve_dependencies(
            request=request,
            dependant=dependant,
            body=body,
            dependency_overrides_provider=dependency_overrides_provider,
            async_exit_stack=async_exit_stack,
            embed_body_fields=embed_body_fields,
        )
        errors = solved_result.errors
        if not errors:
            raw_response = await run_endpoint_function(
                dependant=dependant,
                values=solved_result.values,
                is_coroutine=is_coroutine,
            )
            if isinstance(raw_response, Response):
                if raw_response.background is None:
                    raw_response.background = solved_result.background_tasks
                response = raw_response
            else:
                response_args: dict[str, Any] = {
                    "background": solved_result.background_tasks
                }
                # If status_code was set, use it, otherwise use the default from the
                # response class, in the case of redirect it's 307.
                # Response-level status takes priority over route-level.
                current_status_code = (
                    solved_result.response.status_code or status_code
                )
                if current_status_code is not None:
                    response_args["status_code"] = current_status_code
                # Fast path: skip serialize_response for simple types
                # when no response model filtering is needed
                if (
                    response_field is None
                    and isinstance(raw_response, (dict, list, str, int, float, bool))
                ):
                    content = raw_response
                else:
                    content = await serialize_response(
                        field=response_field,
                        response_content=raw_response,
                        include=response_model_include,
                        exclude=response_model_exclude,
                        by_alias=response_model_by_alias,
                        exclude_unset=response_model_exclude_unset,
                        exclude_defaults=response_model_exclude_defaults,
                        exclude_none=response_model_exclude_none,
                        is_coroutine=is_coroutine,
                        endpoint_ctx=endpoint_ctx,
                    )
                # Core fast-path: encode + serialize to JSON bytes in one
                # GIL-releasing call, skipping JSONResponse.render()
                if actual_response_class is JSONResponse:
                    try:
                        body_bytes = encode_to_json_bytes(content)
                        response = Response(
                            content=body_bytes,
                            status_code=response_args.get(
                                "status_code", 200
                            ),
                            media_type="application/json",
                            background=response_args.get("background"),
                        )
                    except (ValueError, TypeError):
                        response = actual_response_class(
                            content, **response_args
                        )
                else:
                    response = actual_response_class(content, **response_args)
                if not is_body_allowed_for_status_code(response.status_code):
                    response.body = b""
                response.headers.raw.extend(solved_result.response.headers.raw)
        if errors:
            validation_error = RequestValidationError(
                errors, body=body, endpoint_ctx=endpoint_ctx
            )
            raise validation_error

        # Return response
        assert response
        return response

    return app


def get_websocket_app(
    dependant: Dependant,
    dependency_overrides_provider: Optional[Any] = None,
    embed_body_fields: bool = False,
) -> Callable[[WebSocket], Coroutine[Any, Any, Any]]:
    async def app(websocket: WebSocket) -> None:
        endpoint_ctx = (
            _extract_endpoint_context(dependant.call)
            if dependant.call
            else EndpointContext()
        )
        if dependant.path:
            # For mounted sub-apps, include the mount path prefix
            mount_path = websocket.scope.get("root_path", "").rstrip("/")
            endpoint_ctx["path"] = f"WS {mount_path}{dependant.path}"
        async_exit_stack = websocket.scope.get("fastapi_inner_astack")
        assert isinstance(async_exit_stack, AsyncExitStack), (
            "fastapi_inner_astack not found in request scope"
        )
        solved_result = await solve_dependencies(
            request=websocket,
            dependant=dependant,
            dependency_overrides_provider=dependency_overrides_provider,
            async_exit_stack=async_exit_stack,
            embed_body_fields=embed_body_fields,
        )
        if solved_result.errors:
            raise WebSocketRequestValidationError(
                solved_result.errors,
                endpoint_ctx=endpoint_ctx,
            )
        assert dependant.call is not None, "dependant.call must be a function"
        await dependant.call(**solved_result.values)

    return app


class APIWebSocketRoute(routing.WebSocketRoute):
    def __init__(
        self,
        path: str,
        endpoint: Callable[..., Any],
        *,
        name: Optional[str] = None,
        dependencies: Optional[Sequence[params.Depends]] = None,
        dependency_overrides_provider: Optional[Any] = None,
    ) -> None:
        self.path = path
        self.endpoint = endpoint
        self.name = get_name(endpoint) if name is None else name
        self.dependencies = list(dependencies or [])
        self.path_regex, self.path_format, self.param_convertors = compile_path(path)
        self.dependant = get_dependant(
            path=self.path_format, call=self.endpoint, scope="function"
        )
        for depends in self.dependencies[::-1]:
            self.dependant.dependencies.insert(
                0,
                get_parameterless_sub_dependant(depends=depends, path=self.path_format),
            )
        self._flat_dependant = get_flat_dependant(self.dependant)
        self._embed_body_fields = _should_embed_body_fields(
            self._flat_dependant.body_params
        )
        self.app = websocket_session(
            get_websocket_app(
                dependant=self.dependant,
                dependency_overrides_provider=dependency_overrides_provider,
                embed_body_fields=self._embed_body_fields,
            )
        )

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        match, child_scope = super().matches(scope)
        if match != Match.NONE:
            child_scope["route"] = self
        return match, child_scope


def _build_field_specs(
    dependant: Dependant,
) -> list[tuple[str, str, str, str, bool, bool, bool, bool, Any]]:
    """
    Build field specs for Core batch parameter extraction.

    Returns a list of tuples:
        (location, alias, type_tag, field_name, is_sequence, is_json,
         convert_underscores, required, default_value)

    Called once at route registration time (startup), not per-request.
    For model-based params (query/header with BaseModel type), expands to inner fields.
    """
    from pydantic import BaseModel as _BaseModel
    from fastapi._compat import is_sequence_field, get_cached_model_fields
    from fastapi.dependencies.utils import _get_scalar_type_tag, _is_json_field, get_validation_alias

    specs: list[tuple[str, str, str, str, bool, bool, bool, bool, Any]] = []

    for field in dependant.query_params:
        if lenient_issubclass(field.type_, _BaseModel):
            # Model-based query param: expand inner fields so C++ extracts them
            default_cu = getattr(field.field_info, "convert_underscores", False)
            for inner_f in get_cached_model_fields(field.type_):
                inner_alias = get_validation_alias(inner_f)
                tag = _get_scalar_type_tag(inner_f) or ""
                is_seq = is_sequence_field(inner_f) and not _is_json_field(inner_f)
                is_json = _is_json_field(inner_f)
                # Mark as not-required: the outer model validator handles "required"
                inner_default = None if inner_f.required else inner_f.get_default()
                specs.append(("query", inner_alias, tag, inner_f.name, is_seq, is_json, False, False, inner_default))
        else:
            alias = get_validation_alias(field)
            tag = _get_scalar_type_tag(field) or ""
            is_seq = is_sequence_field(field) and not _is_json_field(field)
            is_json = _is_json_field(field)
            is_req = field.required
            default = None if is_req else field.get_default()
            specs.append(("query", alias, tag, field.name, is_seq, is_json, False, is_req, default))

    for field in dependant.header_params:
        if lenient_issubclass(field.type_, _BaseModel):
            # Model-based header param: expand inner fields
            default_cu = getattr(field.field_info, "convert_underscores", True)
            for inner_f in get_cached_model_fields(field.type_):
                inner_alias = get_validation_alias(inner_f)
                tag = _get_scalar_type_tag(inner_f) or ""
                is_seq = is_sequence_field(inner_f) and not _is_json_field(inner_f)
                # Inner fields always use convert_underscores=True (match both dash and underscore)
                convert = True
                inner_default = None if inner_f.required else inner_f.get_default()
                # seq_underscore_only: when outer convert_underscores=False, only collect into list
                # if the header name has underscores (not dashes)
                seq_uo = is_seq and not default_cu
                spec_dict = ("header", inner_alias, tag, inner_f.name, is_seq, False, convert, False, inner_default)
                specs.append(spec_dict)
                # Store seq_underscore_only flag for C++ (appended as extra element)
                if seq_uo:
                    specs[-1] = spec_dict + (seq_uo,)
        else:
            alias = get_validation_alias(field)
            tag = _get_scalar_type_tag(field) or ""
            convert = getattr(field.field_info, "convert_underscores", True)
            is_seq = is_sequence_field(field) and not _is_json_field(field)
            is_req = field.required
            default = None if is_req else field.get_default()
            specs.append(("header", alias, tag, field.name, is_seq, False, convert, is_req, default))

    for field in dependant.cookie_params:
        if lenient_issubclass(field.type_, _BaseModel):
            # Model-based cookie param: expand inner fields
            for inner_f in get_cached_model_fields(field.type_):
                inner_alias = get_validation_alias(inner_f)
                tag = _get_scalar_type_tag(inner_f) or ""
                inner_default = None if inner_f.required else inner_f.get_default()
                specs.append(("cookie", inner_alias, tag, inner_f.name, False, False, False, False, inner_default))
        else:
            alias = get_validation_alias(field)
            tag = _get_scalar_type_tag(field) or ""
            is_req = field.required
            default = None if is_req else field.get_default()
            specs.append(("cookie", alias, tag, field.name, False, False, False, is_req, default))

    for field in dependant.path_params:
        alias = get_validation_alias(field)
        tag = _get_scalar_type_tag(field) or ""
        # Path params are always required by definition
        specs.append(("path", alias, tag, field.name, False, False, False, True, None))

    return specs


def _make_param_validator(dependant: Dependant) -> Optional[Any]:
    """Create a fast param validator callable for the C++ fast path.

    Returns a callable (kwargs_dict) → (validated_values, errors_list) that:
    1. Checks for missing required params (returns 422 "Field required" error)
    2. Validates all params using Pydantic TypeAdapters (catches min_length, ge, etc.)
    3. Returns coerced/validated values to be merged into kwargs

    Returns None if there are no params to validate (no-op route).
    Called once at route registration time; returned callable runs per-request.
    """
    from pydantic import BaseModel as _BaseModel
    from fastapi._compat import get_cached_model_fields
    from fastapi.dependencies.utils import get_validation_alias

    # Separate regular scalar params from model-based params
    # Regular: (loc, alias, field_name, is_required, default, type_adapter)
    param_info = []
    # Model-based: (loc, outer_field_name, outer_ta, [(inner_name, inner_alias)])
    model_param_info = []

    for loc, fields in [
        ("query", dependant.query_params),
        ("header", dependant.header_params),
        ("cookie", dependant.cookie_params),
        ("path", dependant.path_params),
    ]:
        for field in fields:
            if lenient_issubclass(field.type_, _BaseModel):
                # Model-based param — the C++ extracts inner fields individually.
                # We collect them here to assemble + validate the outer model.
                default_cu = getattr(field.field_info, "convert_underscores", True)
                inner_specs = []
                for inner_f in get_cached_model_fields(field.type_):
                    inner_alias = get_validation_alias(inner_f)
                    from fastapi.dependencies.utils import is_sequence_field as _isf
                    inner_is_list = _isf(inner_f)
                    inner_specs.append((inner_f.name, inner_alias, inner_is_list))
                model_param_info.append((loc, field.name, field._type_adapter, inner_specs, default_cu))
            else:
                alias = get_validation_alias(field)
                is_req = field.required
                default = None if is_req else field.get_default()
                param_info.append((loc, alias, field.name, is_req, default, field._type_adapter))

    if not param_info and not model_param_info:
        return None

    # Pre-import ValidationError for zero-import-overhead in the hot path
    from pydantic import ValidationError as _PydanticValidationError

    def _validate(kwargs_dict: dict) -> tuple[dict, list]:
        # Pop C++-injected keys that shouldn't be treated as request params
        _raw_headers_saved = kwargs_dict.pop('__raw_headers__', None)
        kwargs_dict.pop('__method__', None)
        kwargs_dict.pop('__path__', None)
        kwargs_dict.pop('__auth_scheme__', None)
        kwargs_dict.pop('__auth_credentials__', None)

        values: dict = {}
        errors: list = []

        for loc, alias, name, is_req, default, ta in param_info:
            # Look up by alias first (how C++ stores it), then by field name
            value = kwargs_dict.get(alias)
            if value is None and alias != name:
                value = kwargs_dict.get(name)

            if value is None:
                if is_req:
                    errors.append({
                        "type": "missing",
                        "loc": (loc, alias),
                        "msg": "Field required",
                        "input": None,
                    })
                else:
                    # Optional field absent from request — inject the default value
                    # (which may be None). This prevents the Python function's
                    # default parameter (often a FieldInfo object) from leaking to
                    # the endpoint as an unexpected value.
                    values[name] = default
                continue

            # Validate + type-coerce using the pre-built TypeAdapter
            try:
                validated = ta.validate_python(value)
                values[name] = validated
            except _PydanticValidationError as e:
                for err in e.errors(include_url=False):
                    new_err = dict(err)
                    inner_loc = new_err.pop("loc", ())
                    new_err["loc"] = (loc, alias) + tuple(inner_loc)
                    errors.append(new_err)

        for _mpi in model_param_info:
            loc, outer_name, outer_ta, inner_specs = _mpi[0], _mpi[1], _mpi[2], _mpi[3]
            _cu = _mpi[4] if len(_mpi) > 4 else True
            # Collect inner field values from kwargs (C++ extracted them individually)
            inner_dict: dict = {}
            known_keys: set = set()
            for inner_name, inner_alias, inner_is_list in inner_specs:
                known_keys.add(inner_name)
                known_keys.add(inner_alias)
                val = kwargs_dict.get(inner_name)
                if val is None and inner_alias != inner_name:
                    val = kwargs_dict.get(inner_alias)
                if val is not None:
                    # Wrap string in list for list-type fields (only when convert_underscores=True)
                    if inner_is_list and isinstance(val, str) and _cu:
                        val = [val]
                    inner_dict[inner_name] = val
            # Include extra params so extra="forbid" models can reject them
            if loc == "query":
                try:
                    from fastapi._cpp_server import _current_query_string as _cqs
                    from urllib.parse import parse_qs as _pqs
                    _qs_raw = _cqs.get()
                    if _qs_raw:
                        _qs_all = _pqs(_qs_raw.decode('latin-1'), keep_blank_values=True)
                        for _k, _vs in _qs_all.items():
                            if _k not in known_keys and _k not in inner_dict:
                                inner_dict[_k] = _vs[0] if len(_vs) == 1 else _vs
                except Exception:
                    pass
            elif loc == "cookie":
                # Parse extra cookies from raw Cookie header
                _cookie_hdrs = _raw_headers_saved
                if not _cookie_hdrs:
                    try:
                        from fastapi._cpp_server import _current_raw_headers as _crh3
                        _cookie_hdrs = _crh3.get()
                    except Exception: pass
                if _cookie_hdrs:
                    try:
                        from http.cookies import SimpleCookie as _SC
                        for _hname, _hval in _cookie_hdrs:
                            _hn = (_hname.decode('latin-1') if isinstance(_hname, bytes) else _hname).lower()
                            if _hn == 'cookie':
                                _hv = _hval.decode('latin-1') if isinstance(_hval, bytes) else _hval
                                _sc = _SC()
                                _sc.load(_hv)
                                for _ck, _cv in _sc.items():
                                    if _ck not in known_keys and _ck not in inner_dict:
                                        inner_dict[_ck] = _cv.value
                    except Exception:
                        pass
            elif loc == "header":
                _raw_hdrs = _raw_headers_saved
                if not _raw_hdrs:
                    try:
                        from fastapi._cpp_server import _current_raw_headers as _crh2
                        _raw_hdrs = _crh2.get()
                    except Exception: pass
                if _raw_hdrs:
                    for _hname, _hval in _raw_hdrs:
                        try:
                            _hn = (_hname.decode('latin-1') if isinstance(_hname, bytes) else _hname).lower()
                            _hv = _hval.decode('latin-1') if isinstance(_hval, bytes) else _hval
                            # Normalize: replace - with _ to match field names
                            _hn_norm = _hn.replace('-', '_')
                            if _hn_norm not in known_keys and _hn_norm not in inner_dict:
                                # Skip standard HTTP headers that are always present
                                if _hn not in ('host', 'content-length', 'content-type', 'accept', 'accept-encoding', 'user-agent', 'connection'):
                                    inner_dict[_hn_norm] = _hv
                        except Exception:
                            pass

            # Validate the assembled dict as the model type
            try:
                validated = outer_ta.validate_python(inner_dict)
                values[outer_name] = validated
            except _PydanticValidationError as e:
                _errs = e.errors(include_url=False)
                # extra_forbidden errors: for query params report as missing outer param;
                # for headers/cookies keep as extra_forbidden with inner loc
                if loc in ("query", "cookie") and any(err.get("type") == "extra_forbidden" for err in _errs):
                    # For cookies: report missing at first inner field
                    _miss_name = outer_name
                    if loc == "cookie":
                        _miss_name = inner_specs[0][0] if inner_specs else outer_name
                    errors.append({"type": "missing", "loc": (loc, _miss_name), "msg": "Field required", "input": {} if loc == "cookie" else None})
                else:
                    for err in _errs:
                        new_err = dict(err)
                        inner_loc = new_err.pop("loc", ())
                        new_err["loc"] = (loc,) + tuple(inner_loc)
                        errors.append(new_err)

        # Collect consumed raw keys (individual fields assembled into models)
        consumed: set = set()
        for _, alias, name, _, _, _ in param_info:
            consumed.add(alias)
            consumed.add(name)
        for _mpi2 in model_param_info:
            outer_name, inner_specs = _mpi2[1], _mpi2[3]
            for inner_name, inner_alias, _ in inner_specs:
                consumed.add(inner_name)
                consumed.add(inner_alias)
        return (values, errors, consumed)

    return _validate


def _build_dependency_graph(
    dependant: Dependant,
) -> list[tuple[str, list[str]]]:
    """
    Build a flat dependency graph from a Dependant tree for Core topological sort.

    Returns a list of (name, [dependency_names]) tuples representing the graph.
    Called once at route registration time (startup), not per-request.
    """
    result: list[tuple[str, list[str]]] = []
    _collect_deps(dependant, result, set())
    return result


def _collect_deps(
    dependant: Dependant,
    result: list[tuple[str, list[str]]],
    visited: set[str],
) -> None:
    """Recursively collect dependency names and their sub-dependency names."""
    for i, sub_dep in enumerate(dependant.dependencies):
        name = sub_dep.name or getattr(sub_dep.call, '__name__', None) or f'__dep_{i}'
        if name not in visited:
            visited.add(name)
            sub_names = [
                d.name or getattr(d.call, '__name__', None) or f'__subdep_{j}'
                for j, d in enumerate(sub_dep.dependencies)
                if d.call is not None
            ]
            result.append((name, sub_names))
            _collect_deps(sub_dep, result, visited)


def _flatten_deps(
    dependant: Dependant,
    result: list[dict],
    visited_ids: set,
) -> None:
    """Recursively flatten the dependency tree into topological order (leaves first).

    Walks the Dependant tree depth-first: children are appended before parents,
    ensuring that when we resolve in list order, every dependency's sub-dependencies
    have already been resolved.

    Raises ValueError if a generator dependency is encountered (not supported
    in C++ fast path — needs AsyncExitStack).

    Uses cache_key (call + scopes) for deduplication, matching FastAPI semantics:
    Security(f, scopes=["a"]) and Security(f, scopes=["b"]) are different nodes.
    Dependencies with use_cache=False are always added as fresh nodes (not deduped).
    """
    for i, sub_dep in enumerate(dependant.dependencies):
        if sub_dep.call is None:
            continue

        use_cache = sub_dep.use_cache
        dep_cache_key = sub_dep.cache_key  # (call, scopes_tuple, computed_scope)

        if dep_cache_key in visited_ids and use_cache:
            # Same callable with same scopes already in dep_nodes (and caching allowed).
            # Record the name as an alias so resolvers can key the result by it.
            if sub_dep.name:
                for node in result:
                    if node.get('cache_key') == dep_cache_key and node.get('use_cache', True):
                        aliases: list = node.setdefault('aliases', [])
                        if sub_dep.name not in aliases:
                            aliases.append(sub_dep.name)
                        break
            continue

        # For use_cache=True, mark as visited to prevent future duplicates.
        # For use_cache=False, DON'T add to visited_ids so this callable can also
        # appear from other use_cache=False occurrences.
        if use_cache:
            visited_ids.add(dep_cache_key)

        # Recurse into children FIRST (ensures topological order: leaves before parents).
        # For use_cache=False nodes, children were already handled on first occurrence;
        # skip recursion to avoid double-processing.
        if use_cache or dep_cache_key not in visited_ids:
            _flatten_deps(sub_dep, result, visited_ids)

        # Auto-generate name if None (fixes router-level deps with name=None)
        dep_id = id(sub_dep.call)
        name = sub_dep.name
        if not name:
            name = getattr(sub_dep.call, '__name__', None) or f'__dep_{i}_{dep_id}'

        # Collect child dependency names (these map to parameters in the parent callable)
        child_names = []
        for child in sub_dep.dependencies:
            child_name = child.name
            if not child_name:
                child_name = getattr(child.call, '__name__', None) or \
                    f'__dep_{id(child.call)}'
            child_names.append(child_name)

        # Collect param names this dep needs from request kwargs
        param_names: set[str] = set()
        required_query_params: set[str] = set()  # required query params (no default)
        required_body_params: set[str] = set()   # required body params (no default)
        required_header_params: set[str] = set()  # required header params
        required_cookie_params: set[str] = set()  # required cookie params
        for f in sub_dep.query_params:
            param_names.add(f.name)
            if f.required:
                required_query_params.add(f.name)
        for f in sub_dep.header_params:
            param_names.add(f.name)
            if f.required:
                required_header_params.add(f.alias or f.name)
        for f in sub_dep.cookie_params:
            param_names.add(f.name)
            if f.required:
                required_cookie_params.add(f.alias or f.name)
        for f in sub_dep.path_params:
            param_names.add(f.name)
        has_form_body = False
        for f in sub_dep.body_params:
            param_names.add(f.name)
            if f.required:
                required_body_params.add(f.name)
            if isinstance(f.field_info, params.Form):
                has_form_body = True

        # Detect security scheme deps (HTTPBearer, HTTPBasic, etc.)
        # These are resolved natively by C++ auth extraction — no Starlette needed
        from fastapi.security.http import HTTPBase
        is_security = isinstance(sub_dep.call, HTTPBase)
        security_type = type(sub_dep.call).__name__ if is_security else None
        auto_error = getattr(sub_dep.call, 'auto_error', True) if is_security else True
        # Pre-bake credential class references so resolvers need zero imports per call
        security_cred_cls = None
        if is_security:
            if security_type in ('HTTPBearer', 'HTTPBase'):
                security_cred_cls = _HTTPAuthCreds
            elif security_type == 'HTTPBasic':
                security_cred_cls = _HTTPBasicCreds

        result.append({
            'name': name,
            'call': sub_dep.call,
            'cache_key': dep_cache_key,
            'use_cache': use_cache,
            'is_coro': sub_dep.is_coroutine_callable,
            'is_gen': getattr(sub_dep, 'is_gen_callable', False),
            'is_async_gen': getattr(sub_dep, 'is_async_gen_callable', False),
            'needs_request': bool(sub_dep.request_param_name),
            'request_param_name': sub_dep.request_param_name,
            'needs_http_connection': bool(sub_dep.http_connection_param_name),
            'http_connection_param_name': sub_dep.http_connection_param_name,
            'child_names': child_names,
            'param_names': param_names,
            'required_query_params': required_query_params,
            'required_body_params': required_body_params,
            'has_form_body': has_form_body,
            'required_header_params': required_header_params,
            'required_cookie_params': required_cookie_params,
            'security_scopes_param': sub_dep.security_scopes_param_name,
            'oauth_scopes': list(sub_dep.parent_oauth_scopes or []) + list(sub_dep.own_oauth_scopes or []),
            'is_security': is_security,
            'security_type': security_type,
            'auto_error': auto_error,
            'security_cred_cls': security_cred_cls,
            'response_param_name': sub_dep.response_param_name,
            'background_tasks_param_name': sub_dep.background_tasks_param_name,
        })


def _make_lightweight_request(raw_headers, method, path, app=None, route_id=None, _shim_route=None):
    """Create a lightweight Starlette Request from raw headers for security deps.

    Security dependencies (HTTPBearer, HTTPBasic, etc.) access request.headers
    to extract Authorization tokens. This builds a minimal Request from the
    raw header tuples that C++ injected into kwargs.
    """
    # Use the FastAPI Request class (already imported at module level).
    # raw_headers is list of (name_bytes, value_bytes) from C++.
    # ASGI requires header names as lowercase bytes. C++ passes them as-is
    # from the HTTP parser (e.g., b"Authorization"), so we must lowercase here.
    header_list = []
    if raw_headers:
        for item in raw_headers:
            if item is None:
                continue
            name, value = item
            if isinstance(name, bytes):
                name = name.lower()
            elif isinstance(name, str):
                name = name.lower().encode('latin-1')
            if isinstance(value, str):
                value = value.encode('latin-1')
            header_list.append((name, value))

    # Get body bytes from ContextVar for request.body() support
    _body_bytes = b''
    try:
        from fastapi._cpp_server import _current_body as _cb
        _body_bytes = _cb.get() or b''
    except Exception:
        pass

    _root_path = ''
    try:
        from fastapi._cpp_server import _server_root_path as _srp
        _root_path = _srp
    except Exception:
        pass

    # Extract host from headers for server tuple
    _host_hdr = None
    _scheme_hdr = 'http'
    for _hn, _hv in (header_list or []):
        _hn_s = _hn.lower() if isinstance(_hn, str) else _hn.lower().decode('latin-1')
        _hv_s = _hv if isinstance(_hv, str) else _hv.decode('latin-1')
        if _hn_s == 'host':
            _host_hdr = _hv_s
        elif _hn_s == 'x-forwarded-proto':
            _scheme_hdr = _hv_s
    _server_tuple = None
    if _host_hdr:
        _h_parts = _host_hdr.rsplit(':', 1)
        _s_host = _h_parts[0]
        _s_port = int(_h_parts[1]) if len(_h_parts) > 1 else (443 if _scheme_hdr == 'https' else 80)
        _server_tuple = (_s_host, _s_port)
    scope = {
        "type": "http",
        "method": method or "GET",
        "path": path or "/",
        "query_string": b"",
        "headers": header_list,
        "root_path": _root_path,
        "_body": _body_bytes,
        "scheme": _scheme_hdr,
        "server": _server_tuple,
        "client": ("testclient", 50000) if any(
            (n.lower() if isinstance(n, str) else n.lower().decode("latin-1")) == "user-agent" and
            (v if isinstance(v, str) else v.decode("latin-1")).lower() == "testclient"
            for n, v in (header_list or [])
        ) else ("127.0.0.1", 0),
    }
    if app is not None:
        scope["app"] = app
        if hasattr(app, 'router'):
            scope["router"] = app.router
    # Inject route for scope["route"] access in endpoints
    _route = _shim_route
    if _route is None and route_id is not None:
        _route = _route_id_to_route.get(route_id)
    if _route is None:
        try:
            from fastapi._cpp_server import _current_route as _cr
            _route = _cr.get()
        except Exception:
            pass
    if _route is not None:
        scope["route"] = _route
    _req = Request(scope)
    if _body_bytes:
        _req._body = _body_bytes
    return _req


def _resolve_deps_sync(dep_nodes, kwargs_dict, _exc_types=_DEP_HTTP_EXC_TYPES, _app=None):
    """Resolve dependencies in topological order (sync version).

    Uses module-level pre-imported constants (_DEP_HTTP_EXC_TYPES, _HTTPAuthCreds,
    _HTTPBasicCreds, _base64) to eliminate sys.modules lookups on every request.
    """
    resolved = {}
    errors = []
    sub_response = None

    # Pop C++-injected request info from kwargs (prevent leaking to endpoint)
    raw_headers = kwargs_dict.pop('__raw_headers__', None)
    raw_method = kwargs_dict.pop('__method__', 'GET')
    raw_path = kwargs_dict.pop('__path__', '/')
    auth_scheme = kwargs_dict.pop('__auth_scheme__', None)
    auth_credentials = kwargs_dict.pop('__auth_credentials__', None)
    raw_body_bytes = kwargs_dict.pop('__body__', None)
    raw_content_type = kwargs_dict.pop('__content_type__', '')
    # Fallback to ContextVar when C++ dep engine already popped __raw_headers__
    if raw_headers is None:
        try:
            from fastapi._cpp_server import _current_raw_headers as _crh, _current_method as _cm, _current_path as _cp
            raw_headers = _crh.get()
            if raw_method == 'GET': raw_method = _cm.get()
            if raw_path == '/': raw_path = _cp.get()
        except Exception:
            pass
    # Parse form body once if any dep node needs it
    _parsed_form: dict | None = None
    if raw_body_bytes and any(n.get('has_form_body') for n in dep_nodes):
        ct = raw_content_type if isinstance(raw_content_type, str) else ''
        if 'application/x-www-form-urlencoded' in ct:
            from fastapi._fastapi_core import parse_urlencoded_body as _pub
            _parsed_form = dict(_pub(raw_body_bytes))
        elif 'multipart/form-data' in ct:
            _bnd = _extract_boundary(ct)
            if _bnd:
                _parts = parse_multipart_body(raw_body_bytes, _bnd)
                _fd = _build_form_data(_parts)
                _parsed_form = dict(_fd)
    request_obj = None

    for node in dep_nodes:
        try:
            # C++ native security extraction — no Starlette Request needed
            if node['is_security']:
                sec_type = node['security_type']
                if sec_type in ('HTTPBearer', 'HTTPBase'):
                    if not auth_scheme or auth_scheme.lower() != 'bearer' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _www_auth = getattr(_sec_obj, 'model', None)
                            _scheme = getattr(_www_auth, 'scheme', 'Bearer') or 'Bearer'
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _scheme.title()})
                        creds_val = None
                    else:
                        # Use pre-baked class ref — zero import overhead
                        creds_val = node['security_cred_cls'](
                            scheme=auth_scheme, credentials=auth_credentials)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue
                elif sec_type == 'HTTPBasic':
                    if not auth_scheme or auth_scheme.lower() != 'basic' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _realm = getattr(_sec_obj, 'realm', None)
                            _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _www_auth_hdr})
                        creds_val = None
                    else:
                        _sec_obj = node['call']
                        _realm = getattr(_sec_obj, 'realm', None)
                        _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                        try:
                            decoded = _base64.b64decode(auth_credentials).decode('utf-8')
                        except Exception:
                            if node['auto_error']:
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                            resolved[node['name']] = creds_val
                            continue
                        username, sep, password = decoded.partition(':')
                        if not sep:
                            if node['auto_error']:
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                        else:
                            creds_val = node['security_cred_cls'](username=username, password=password)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue

            dep_kwargs = {}

            # Inject resolved child dependencies
            for child_name in node['child_names']:
                if child_name in resolved:
                    dep_kwargs[child_name] = resolved[child_name]

            # Inject request params from the original kwargs
            for pname in node['param_names']:
                if pname in kwargs_dict:
                    dep_kwargs[pname] = kwargs_dict[pname]
            # Inject form body fields if this dep has form params
            if node.get('has_form_body') and _parsed_form:
                for pname in node['param_names']:
                    if pname not in dep_kwargs and pname in _parsed_form:
                        dep_kwargs[pname] = _parsed_form[pname]

            # Validate required query params -- produce proper Pydantic-style errors
            _prev_err_count = len(errors)
            _req_qp = node.get('required_query_params')
            if _req_qp:
                for _rp in _req_qp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("query", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required body params
            _req_bp = node.get('required_body_params')
            if _req_bp:
                for _rp in _req_bp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("body", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required header params
            _req_hp = node.get('required_header_params')
            if _req_hp:
                for _rp in _req_hp:
                    if _rp not in dep_kwargs and _rp.replace('-', '_') not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("header", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required cookie params
            _req_cp = node.get('required_cookie_params')
            if _req_cp:
                for _rp in _req_cp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("cookie", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue

            # Inject Request object if needed (fallback for non-security deps)
            if node['needs_request'] or node['needs_http_connection']:
                if request_obj is None:
                    request_obj = _make_lightweight_request(
                        raw_headers, raw_method, raw_path, _app)
                param_name = node['request_param_name'] or \
                    node['http_connection_param_name']
                if param_name:
                    dep_kwargs[param_name] = request_obj

            # Inject Response object if dep declares response param
            _resp_param = node.get('response_param_name')
            if _resp_param:
                if sub_response is None:
                    sub_response = Response()
                dep_kwargs[_resp_param] = sub_response

            # Inject BackgroundTasks if dep declares background_tasks param
            _bt_param = node.get('background_tasks_param_name')
            if _bt_param:
                from fastapi.background import BackgroundTasks as _BT
                if '__bg_tasks__' not in kwargs_dict:
                    kwargs_dict['__bg_tasks__'] = _BT()
                dep_kwargs[_bt_param] = kwargs_dict['__bg_tasks__']

            # Inject SecurityScopes if dep declares security_scopes param
            _ss_param = node.get('security_scopes_param')
            if _ss_param:
                from fastapi.security.oauth2 import SecurityScopes as _SecurityScopes
                dep_kwargs[_ss_param] = _SecurityScopes(scopes=node.get('oauth_scopes') or [])

            result = node['call'](**dep_kwargs)
            resolved[node['name']] = result
            for alias in node.get('aliases', ()):
                resolved[alias] = result

        except _exc_types:
            raise  # Propagate auth/permission errors — C++ handles these
        except Exception as exc:
            errors.append({
                "loc": ("dependency", node['name']),
                "msg": str(exc),
                "type": "dependency_error",
            })

    return (resolved, errors, sub_response)


async def _resolve_deps_async(dep_nodes, kwargs_dict, _exc_types=_DEP_HTTP_EXC_TYPES, _app=None):
    """Resolve dependencies in topological order (async version).

    Uses module-level pre-imported constants (_DEP_HTTP_EXC_TYPES, _HTTPAuthCreds,
    _HTTPBasicCreds, _base64) to eliminate sys.modules lookups on every request.
    """
    resolved = {}
    errors = []
    sub_response = None

    # Pop C++-injected request info from kwargs (prevent leaking to endpoint)
    raw_headers = kwargs_dict.pop('__raw_headers__', None)
    raw_method = kwargs_dict.pop('__method__', 'GET')
    raw_path = kwargs_dict.pop('__path__', '/')
    auth_scheme = kwargs_dict.pop('__auth_scheme__', None)
    auth_credentials = kwargs_dict.pop('__auth_credentials__', None)
    raw_body_bytes = kwargs_dict.pop('__body__', None)
    raw_content_type = kwargs_dict.pop('__content_type__', '')
    # Fallback to ContextVar when C++ dep engine already popped __raw_headers__
    if raw_headers is None:
        try:
            from fastapi._cpp_server import _current_raw_headers as _crh, _current_method as _cm, _current_path as _cp
            raw_headers = _crh.get()
            if raw_method == 'GET': raw_method = _cm.get()
            if raw_path == '/': raw_path = _cp.get()
        except Exception:
            pass
    # Parse form body once if any dep node needs it
    _parsed_form: dict | None = None
    if raw_body_bytes and any(n.get('has_form_body') for n in dep_nodes):
        ct = raw_content_type if isinstance(raw_content_type, str) else ''
        if 'application/x-www-form-urlencoded' in ct:
            from fastapi._fastapi_core import parse_urlencoded_body as _pub
            _parsed_form = dict(_pub(raw_body_bytes))
        elif 'multipart/form-data' in ct:
            _bnd = _extract_boundary(ct)
            if _bnd:
                _parts = parse_multipart_body(raw_body_bytes, _bnd)
                _fd = _build_form_data(_parts)
                _parsed_form = dict(_fd)
    request_obj = None

    for node in dep_nodes:
        try:
            # C++ native security extraction — no Starlette Request needed
            if node['is_security']:
                sec_type = node['security_type']
                if sec_type in ('HTTPBearer', 'HTTPBase'):
                    if not auth_scheme or auth_scheme.lower() != 'bearer' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _www_auth = getattr(_sec_obj, 'model', None)
                            _scheme = getattr(_www_auth, 'scheme', 'Bearer') or 'Bearer'
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _scheme.title()})
                        creds_val = None
                    else:
                        # Use pre-baked class ref — zero import overhead
                        creds_val = node['security_cred_cls'](
                            scheme=auth_scheme, credentials=auth_credentials)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue
                elif sec_type == 'HTTPBasic':
                    if not auth_scheme or auth_scheme.lower() != 'basic' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _realm = getattr(_sec_obj, 'realm', None)
                            _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _www_auth_hdr})
                        creds_val = None
                    else:
                        _sec_obj = node['call']
                        _realm = getattr(_sec_obj, 'realm', None)
                        _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                        try:
                            decoded = _base64.b64decode(auth_credentials).decode('utf-8')
                        except Exception:
                            if node['auto_error']:
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                            resolved[node['name']] = creds_val
                            continue
                        username, sep, password = decoded.partition(':')
                        if not sep:
                            if node['auto_error']:
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                        else:
                            creds_val = node['security_cred_cls'](username=username, password=password)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue

            dep_kwargs = {}

            # Inject resolved child dependencies
            for child_name in node['child_names']:
                if child_name in resolved:
                    dep_kwargs[child_name] = resolved[child_name]

            # Inject request params from the original kwargs
            for pname in node['param_names']:
                if pname in kwargs_dict:
                    dep_kwargs[pname] = kwargs_dict[pname]
            # Inject form body fields if this dep has form params
            if node.get('has_form_body') and _parsed_form:
                for pname in node['param_names']:
                    if pname not in dep_kwargs and pname in _parsed_form:
                        dep_kwargs[pname] = _parsed_form[pname]

            # Validate required query params -- produce proper Pydantic-style errors
            _prev_err_count = len(errors)
            _req_qp = node.get('required_query_params')
            if _req_qp:
                for _rp in _req_qp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("query", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required body params
            _req_bp = node.get('required_body_params')
            if _req_bp:
                for _rp in _req_bp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("body", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required header params
            _req_hp = node.get('required_header_params')
            if _req_hp:
                for _rp in _req_hp:
                    if _rp not in dep_kwargs and _rp.replace('-', '_') not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("header", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue
            # Validate required cookie params
            _req_cp = node.get('required_cookie_params')
            if _req_cp:
                for _rp in _req_cp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("cookie", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if len(errors) > _prev_err_count:
                    continue

            # Inject Request object if needed (fallback for non-security deps)
            if node['needs_request'] or node['needs_http_connection']:
                if request_obj is None:
                    request_obj = _make_lightweight_request(
                        raw_headers, raw_method, raw_path, _app)
                param_name = node['request_param_name'] or \
                    node['http_connection_param_name']
                if param_name:
                    dep_kwargs[param_name] = request_obj

            # Inject Response object if dep declares response param
            _resp_param = node.get('response_param_name')
            if _resp_param:
                if sub_response is None:
                    sub_response = Response()
                dep_kwargs[_resp_param] = sub_response

            # Inject BackgroundTasks if dep declares background_tasks param
            _bt_param = node.get('background_tasks_param_name')
            if _bt_param:
                from fastapi.background import BackgroundTasks as _BT
                if '__bg_tasks__' not in kwargs_dict:
                    kwargs_dict['__bg_tasks__'] = _BT()
                dep_kwargs[_bt_param] = kwargs_dict['__bg_tasks__']

            # Inject SecurityScopes if dep declares security_scopes param
            _ss_param = node.get('security_scopes_param')
            if _ss_param:
                from fastapi.security.oauth2 import SecurityScopes as _SecurityScopes
                dep_kwargs[_ss_param] = _SecurityScopes(scopes=node.get('oauth_scopes') or [])

            # Call the dependency
            if node['is_coro']:
                result = await node['call'](**dep_kwargs)
            else:
                result = node['call'](**dep_kwargs)
            resolved[node['name']] = result
            for alias in node.get('aliases', ()):
                resolved[alias] = result

        except _exc_types:
            raise  # Propagate auth/permission errors — C++ handles these
        except Exception as exc:
            errors.append({
                "loc": ("dependency", node['name']),
                "msg": str(exc),
                "type": "dependency_error",
            })

    return (resolved, errors, sub_response)


async def _resolve_deps_gen(dep_nodes, kwargs_dict, _exc_types=_DEP_HTTP_EXC_TYPES, _app=None):
    """Resolve dependencies including generator (yield) deps using AsyncExitStack.

    Returns (resolved_dict, errors_list, exit_stack).
    The exit_stack must be closed by the caller after the endpoint returns.
    """
    exit_stack = AsyncExitStack()
    resolved = {}
    errors = []
    sub_response = None

    # Pop C++-injected request info from kwargs
    raw_headers = kwargs_dict.pop('__raw_headers__', None)
    raw_method = kwargs_dict.pop('__method__', 'GET')
    raw_path = kwargs_dict.pop('__path__', '/')
    auth_scheme = kwargs_dict.pop('__auth_scheme__', None)
    auth_credentials = kwargs_dict.pop('__auth_credentials__', None)
    # Fallback to ContextVar when C++ dep engine already popped __raw_headers__
    if raw_headers is None:
        try:
            from fastapi._cpp_server import _current_raw_headers as _crh, _current_method as _cm, _current_path as _cp
            raw_headers = _crh.get()
            if raw_method == 'GET': raw_method = _cm.get()
            if raw_path == '/': raw_path = _cp.get()
        except Exception:
            pass
    request_obj = None

    for node in dep_nodes:
        try:
            if node['is_security']:
                sec_type = node['security_type']
                if sec_type in ('HTTPBearer', 'HTTPBase'):
                    if not auth_scheme or auth_scheme.lower() != 'bearer' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _www_auth = getattr(_sec_obj, 'model', None)
                            _scheme = getattr(_www_auth, 'scheme', 'Bearer') or 'Bearer'
                            await exit_stack.aclose()
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _scheme.title()})
                        creds_val = None
                    else:
                        creds_val = node['security_cred_cls'](
                            scheme=auth_scheme, credentials=auth_credentials)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue
                elif sec_type == 'HTTPBasic':
                    if not auth_scheme or auth_scheme.lower() != 'basic' or not auth_credentials:
                        if node['auto_error']:
                            _sec_obj = node['call']
                            _realm = getattr(_sec_obj, 'realm', None)
                            _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                            await exit_stack.aclose()
                            raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                              headers={"WWW-Authenticate": _www_auth_hdr})
                        creds_val = None
                    else:
                        _sec_obj = node['call']
                        _realm = getattr(_sec_obj, 'realm', None)
                        _www_auth_hdr = f'Basic realm="{_realm}"' if _realm else 'Basic'
                        try:
                            decoded = _base64.b64decode(auth_credentials).decode('utf-8')
                        except Exception:
                            if node['auto_error']:
                                await exit_stack.aclose()
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                            resolved[node['name']] = creds_val
                            continue
                        username, sep, password = decoded.partition(':')
                        if not sep:
                            if node['auto_error']:
                                await exit_stack.aclose()
                                raise _DepHTTPExc(status_code=401, detail="Not authenticated",
                                                  headers={"WWW-Authenticate": _www_auth_hdr})
                            creds_val = None
                        else:
                            creds_val = node['security_cred_cls'](username=username, password=password)
                    resolved[node['name']] = creds_val
                    for alias in node.get('aliases', ()):
                        resolved[alias] = creds_val
                    continue

            dep_kwargs = {}

            for child_name in node['child_names']:
                if child_name in resolved:
                    dep_kwargs[child_name] = resolved[child_name]

            for pname in node['param_names']:
                if pname in kwargs_dict:
                    dep_kwargs[pname] = kwargs_dict[pname]

            _req_qp = node.get('required_query_params')
            if _req_qp:
                for _rp in _req_qp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("query", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if errors:
                    continue
            _req_bp = node.get('required_body_params')
            if _req_bp:
                for _rp in _req_bp:
                    if _rp not in dep_kwargs:
                        errors.append({
                            "type": "missing",
                            "loc": ("body", _rp),
                            "msg": "Field required",
                            "input": None,
                        })
                if errors:
                    continue

            if node['needs_request'] or node['needs_http_connection']:
                if request_obj is None:
                    request_obj = _make_lightweight_request(
                        raw_headers, raw_method, raw_path, _app)
                param_name = node['request_param_name'] or \
                    node['http_connection_param_name']
                if param_name:
                    dep_kwargs[param_name] = request_obj

            # Inject Response object if dep declares response param
            _resp_param = node.get('response_param_name')
            if _resp_param:
                if sub_response is None:
                    sub_response = Response()
                dep_kwargs[_resp_param] = sub_response

            # Inject BackgroundTasks if dep declares background_tasks param
            _bt_param = node.get('background_tasks_param_name')
            if _bt_param:
                from fastapi.background import BackgroundTasks as _BT
                if '__bg_tasks__' not in kwargs_dict:
                    kwargs_dict['__bg_tasks__'] = _BT()
                dep_kwargs[_bt_param] = kwargs_dict['__bg_tasks__']

            # Inject SecurityScopes if dep declares security_scopes param
            _ss_param = node.get('security_scopes_param')
            if _ss_param:
                from fastapi.security.oauth2 import SecurityScopes as _SecurityScopes
                dep_kwargs[_ss_param] = _SecurityScopes(scopes=node.get('oauth_scopes') or [])

            if node.get('is_async_gen'):
                # Async generator dep: wrap as async context manager and enter
                call = node['call']
                cm = asynccontextmanager(call)(**dep_kwargs)
                result = await exit_stack.enter_async_context(cm)
            elif node.get('is_gen'):
                # Sync generator dep: wrap as context manager and enter
                from contextlib import contextmanager as _contextmanager
                call = node['call']
                cm = _contextmanager(call)(**dep_kwargs)
                result = exit_stack.enter_context(cm)
            elif node['is_coro']:
                result = await node['call'](**dep_kwargs)
            else:
                result = node['call'](**dep_kwargs)

            resolved[node['name']] = result
            for alias in node.get('aliases', ()):
                resolved[alias] = result

        except _exc_types:
            await exit_stack.aclose()
            raise
        except Exception as exc:
            errors.append({
                "loc": ("dependency", node['name']),
                "msg": str(exc),
                "type": "dependency_error",
            })

    return (resolved, errors, exit_stack, sub_response)


def _dep_tree_has_special_params(dep: Any) -> bool:
    """Return True if dep or any sub-dep uses special injection params the C++ fast path cannot provide."""
    _SPECIAL = ('request_param_name', 'response_param_name', 'background_tasks_param_name',
                'http_connection_param_name', 'websocket_param_name', 'security_scopes_param_name')
    stack = [dep]
    while stack:
        d = stack.pop()
        if any(getattr(d, attr, None) for attr in _SPECIAL):
            return True
        stack.extend(d.dependencies)
    return False


def _make_dep_solver(dependant: Dependant, dependency_overrides_provider: Optional[Any] = None):
    """Create a fast dependency solver callable for C++ fast path.

    When dependency_overrides_provider is given, overrides are applied at call time
    so app.dependency_overrides changes are respected without re-registration.
    """

    def _apply_overrides(dep, overrides):
        """Recursively replace dep.call with override callable."""
        for sub_dep in dep.dependencies:
            if sub_dep.call is None:
                continue
            override = overrides.get(sub_dep.call)
            if override is not None and override is not sub_dep.call:
                new_sub = get_dependant(
                    path=sub_dep.path or "/",
                    call=override,
                    name=sub_dep.name,
                )
                sub_dep.call = override
                sub_dep.dependencies = new_sub.dependencies
                sub_dep.query_params = new_sub.query_params
                sub_dep.header_params = new_sub.header_params
                sub_dep.cookie_params = new_sub.cookie_params
                sub_dep.path_params = new_sub.path_params
                sub_dep.body_params = new_sub.body_params
                sub_dep.request_param_name = new_sub.request_param_name
                sub_dep.http_connection_param_name = new_sub.http_connection_param_name
                sub_dep.response_param_name = new_sub.response_param_name
                sub_dep.background_tasks_param_name = new_sub.background_tasks_param_name
                sub_dep.security_scopes_param_name = new_sub.security_scopes_param_name
                sub_dep.is_coroutine_callable = new_sub.is_coroutine_callable
            else:
                _apply_overrides(sub_dep, overrides)

    def _build_nodes(dep, overrides, orig_consumed):
        """Build dep_nodes applying overrides, return (nodes, injectable_names, consumed_params)."""
        import copy
        dep_copy = copy.deepcopy(dep)
        if overrides:
            _apply_overrides(dep_copy, overrides)
        nodes: list[dict] = []
        _flatten_deps(dep_copy, nodes, set())
        inj: set[str] = set()
        for sd in dep_copy.dependencies:
            if sd.name and sd.call is not None:
                inj.add(sd.name)
        consumed: set[str] = set(orig_consumed)  # start with original consumed params
        for n in nodes:
            consumed.update(n['param_names'])
        return nodes, inj, consumed

    # Pre-analyze the entire dependency tree at registration time
    dep_nodes: list[dict] = []
    _flatten_deps(dependant, dep_nodes, set())

    if not dep_nodes:
        def _solve_empty(kwargs_dict):
            kwargs_dict.pop('__raw_headers__', None)
            kwargs_dict.pop('__method__', None)
            kwargs_dict.pop('__path__', None)
            kwargs_dict.pop('__auth_scheme__', None)
            kwargs_dict.pop('__auth_credentials__', None)
            return ({}, [])
        return _solve_empty

    injectable_names: set[str] = set()
    for sub_dep in dependant.dependencies:
        if sub_dep.name and sub_dep.call is not None:
            injectable_names.add(sub_dep.name)

    dep_consumed_params: set[str] = set()
    for node in dep_nodes:
        dep_consumed_params.update(node['param_names'])

    # Check for generator (yield) dependencies
    has_gen = any(node.get('is_gen') or node.get('is_async_gen') for node in dep_nodes)

    if has_gen:
        async def _solve_gen_async(kwargs_dict):
            import asyncio
            overrides = (
                getattr(dependency_overrides_provider, 'dependency_overrides', None)
                if dependency_overrides_provider is not None else None
            ) or {}
            if overrides:
                nodes, inj, consumed = _build_nodes(dependant, overrides, dep_consumed_params)
            else:
                nodes, inj, consumed = dep_nodes, injectable_names, dep_consumed_params
            await asyncio.sleep(0)
            resolved, errors, exit_stack, sub_response = await _resolve_deps_gen(nodes, kwargs_dict, _app=dependency_overrides_provider)
            injected = {k: v for k, v in resolved.items() if k in inj} if inj else {}
            for pname in consumed:
                kwargs_dict.pop(pname, None)
            injected['__exit_stack__'] = exit_stack
            return (injected, errors, kwargs_dict.get('__bg_tasks__'), sub_response)
        return _solve_gen_async

    any_async = any(node['is_coro'] for node in dep_nodes)

    if any_async:
        _has_response_deps = any(node.get('response_param_name') for node in dep_nodes)
        async def _solve_async(kwargs_dict):
            if _has_response_deps:
                import asyncio as _asyncio; await _asyncio.sleep(0)  # Force async path for sub_response header merging
            overrides = (
                getattr(dependency_overrides_provider, 'dependency_overrides', None)
                if dependency_overrides_provider is not None else None
            ) or {}
            if overrides:
                nodes, inj, consumed = _build_nodes(dependant, overrides, dep_consumed_params)
                # Inject query params not extracted by C++ (override may need new params)
                try:
                    from fastapi._cpp_server import _current_query_string
                    _qs = _current_query_string.get()
                except Exception:
                    _qs = b''
                if _qs:
                    from urllib.parse import parse_qs
                    for k, vs in parse_qs(_qs.decode('latin-1'), keep_blank_values=True).items():
                        if k not in kwargs_dict:
                            kwargs_dict[k] = vs[0] if len(vs) == 1 else vs
            else:
                nodes, inj, consumed = dep_nodes, injectable_names, dep_consumed_params
            resolved, errors, sub_response = await _resolve_deps_async(nodes, kwargs_dict, _app=dependency_overrides_provider)
            injected = {k: v for k, v in resolved.items() if k in inj} if inj else {}
            for pname in consumed:
                kwargs_dict.pop(pname, None)
            return (injected, errors, kwargs_dict.get('__bg_tasks__'), sub_response)
        return _solve_async
    else:
        def _solve_sync(kwargs_dict):
            overrides = (
                getattr(dependency_overrides_provider, 'dependency_overrides', None)
                if dependency_overrides_provider is not None else None
            ) or {}
            if overrides:
                nodes, inj, consumed = _build_nodes(dependant, overrides, dep_consumed_params)
                try:
                    from fastapi._cpp_server import _current_query_string
                    _qs = _current_query_string.get()
                except Exception:
                    _qs = b''
                if _qs:
                    from urllib.parse import parse_qs
                    for k, vs in parse_qs(_qs.decode('latin-1'), keep_blank_values=True).items():
                        if k not in kwargs_dict:
                            kwargs_dict[k] = vs[0] if len(vs) == 1 else vs
            else:
                nodes, inj, consumed = dep_nodes, injectable_names, dep_consumed_params
            resolved, errors, sub_response = _resolve_deps_sync(nodes, kwargs_dict, _app=dependency_overrides_provider)
            injected = {k: v for k, v in resolved.items() if k in inj} if inj else {}
            for pname in consumed:
                kwargs_dict.pop(pname, None)
            return (injected, errors, kwargs_dict.get('__bg_tasks__'), sub_response)
        return _solve_sync

class APIRoute(routing.Route):
    def __init__(
        self,
        path: str,
        endpoint: Callable[..., Any],
        *,
        response_model: Any = Default(None),
        status_code: Optional[int] = None,
        tags: Optional[list[Union[str, Enum]]] = None,
        dependencies: Optional[Sequence[params.Depends]] = None,
        summary: Optional[str] = None,
        description: Optional[str] = None,
        response_description: str = "Successful Response",
        responses: Optional[dict[Union[int, str], dict[str, Any]]] = None,
        deprecated: Optional[bool] = None,
        name: Optional[str] = None,
        methods: Optional[Union[set[str], list[str]]] = None,
        operation_id: Optional[str] = None,
        response_model_include: Optional[IncEx] = None,
        response_model_exclude: Optional[IncEx] = None,
        response_model_by_alias: bool = True,
        response_model_exclude_unset: bool = False,
        response_model_exclude_defaults: bool = False,
        response_model_exclude_none: bool = False,
        include_in_schema: bool = True,
        response_class: Union[type[Response], DefaultPlaceholder] = Default(
            JSONResponse
        ),
        dependency_overrides_provider: Optional[Any] = None,
        callbacks: Optional[list[BaseRoute]] = None,
        openapi_extra: Optional[dict[str, Any]] = None,
        generate_unique_id_function: Union[
            Callable[["APIRoute"], str], DefaultPlaceholder
        ] = Default(generate_unique_id),
    ) -> None:
        self.path = path
        self.endpoint = endpoint
        if isinstance(response_model, DefaultPlaceholder):
            return_annotation = get_typed_return_annotation(endpoint)
            if lenient_issubclass(return_annotation, Response):
                response_model = None
            else:
                response_model = return_annotation
        self.response_model = response_model
        self.summary = summary
        self.response_description = response_description
        self.deprecated = deprecated
        self.operation_id = operation_id
        self.response_model_include = response_model_include
        self.response_model_exclude = response_model_exclude
        self.response_model_by_alias = response_model_by_alias
        self.response_model_exclude_unset = response_model_exclude_unset
        self.response_model_exclude_defaults = response_model_exclude_defaults
        self.response_model_exclude_none = response_model_exclude_none
        self.include_in_schema = include_in_schema
        self.response_class = response_class
        self.dependency_overrides_provider = dependency_overrides_provider
        self.callbacks = callbacks
        self.openapi_extra = openapi_extra
        self.generate_unique_id_function = generate_unique_id_function
        self.tags = tags or []
        self.responses = responses or {}
        self.name = get_name(endpoint) if name is None else name
        self.path_regex, self.path_format, self.param_convertors = compile_path(path)
        if methods is None:
            methods = ["GET"]
        self.methods: set[str] = {method.upper() for method in methods}
        if isinstance(generate_unique_id_function, DefaultPlaceholder):
            current_generate_unique_id: Callable[[APIRoute], str] = (
                generate_unique_id_function.value
            )
        else:
            current_generate_unique_id = generate_unique_id_function
        self.unique_id = self.operation_id or current_generate_unique_id(self)
        # normalize enums e.g. http.HTTPStatus
        if isinstance(status_code, IntEnum):
            status_code = int(status_code)
        self.status_code = status_code
        if self.response_model:
            assert is_body_allowed_for_status_code(status_code), (
                f"Status code {status_code} must not have a response body"
            )
            response_name = "Response_" + self.unique_id
            if annotation_is_pydantic_v1(self.response_model):
                raise PydanticV1NotSupportedError(
                    "pydantic.v1 models are no longer supported by FastAPI."
                    f" Please update the response model {self.response_model!r}."
                )
            self.response_field = create_model_field(
                name=response_name,
                type_=self.response_model,
                mode="serialization",
            )
        else:
            self.response_field = None  # type: ignore
        self.dependencies = list(dependencies or [])
        self.description = description or inspect.cleandoc(self.endpoint.__doc__ or "")
        # if a "form feed" character (page break) is found in the description text,
        # truncate description text to the content preceding the first "form feed"
        self.description = self.description.split("\f")[0].strip()
        response_fields = {}
        for additional_status_code, response in self.responses.items():
            assert isinstance(response, dict), "An additional response must be a dict"
            model = response.get("model")
            if model:
                assert is_body_allowed_for_status_code(additional_status_code), (
                    f"Status code {additional_status_code} must not have a response body"
                )
                response_name = f"Response_{additional_status_code}_{self.unique_id}"
                if annotation_is_pydantic_v1(model):
                    raise PydanticV1NotSupportedError(
                        "pydantic.v1 models are no longer supported by FastAPI."
                        f" In responses={{}}, please update {model}."
                    )
                response_field = create_model_field(
                    name=response_name, type_=model, mode="serialization"
                )
                response_fields[additional_status_code] = response_field
        if response_fields:
            self.response_fields: dict[Union[int, str], ModelField] = response_fields
        else:
            self.response_fields = {}

        assert callable(endpoint), "An endpoint must be a callable"
        self.dependant = get_dependant(
            path=self.path_format, call=self.endpoint, scope="function"
        )
        for depends in self.dependencies[::-1]:
            self.dependant.dependencies.insert(
                0,
                get_parameterless_sub_dependant(depends=depends, path=self.path_format),
            )
        self._flat_dependant = get_flat_dependant(self.dependant)
        self._embed_body_fields = _should_embed_body_fields(
            self._flat_dependant.body_params
        )
        self.body_field = get_body_field(
            flat_dependant=self._flat_dependant,
            name=self.unique_id,
            embed_body_fields=self._embed_body_fields,
        )
        # Pre-compute dependency resolution order using Core topological sort.
        self._dependency_order: Optional[list[str]] = None
        if self.dependant.dependencies:
            try:
                dep_graph = _build_dependency_graph(self.dependant)
                self._dependency_order = compute_dependency_order(dep_graph)
            except Exception:
                self._dependency_order = None
        # Pre-compute field specs and register with Core param extractor.
        self._core_route_id: Optional[int] = None
        specs = _build_field_specs(self._flat_dependant)
        try:
            if specs:
                route_id = next(_route_id_counter)
                register_route_params(route_id, specs)
                self._core_route_id = route_id
                _route_id_to_route[route_id] = self
                _endpoint_id_to_route[id(self.endpoint)] = self
        except Exception:
            self._core_route_id = None
        # Pre-compute batch specs for solve_dependencies (avoid per-request rebuild)
        # Reuse specs from above if available, otherwise compute only if needed
        if specs is not None:
            self._batch_specs = specs
        elif (
            self._flat_dependant.path_params
            or self._flat_dependant.query_params
            or self._flat_dependant.header_params
            or self._flat_dependant.cookie_params
        ):
            self._batch_specs = _build_field_specs(self._flat_dependant)
        else:
            self._batch_specs = []
        # Determine if route is eligible for the Core ASGI fast path
        actual_rc = self.response_class
        if isinstance(actual_rc, DefaultPlaceholder):
            actual_rc = actual_rc.value
        self._fast_path_eligible: bool = bool(
            self.dependant.call is not None
            and not getattr(self.dependant, 'is_gen_callable', False)
            and not getattr(self.dependant, 'is_async_gen_callable', False)
            and not _dep_tree_has_special_params(self.dependant)
            and not (self.body_field and isinstance(
                self.body_field.field_info, params.Form
            ))
            and isinstance(actual_rc, type) and issubclass(actual_rc, JSONResponse)
        )
        # Build dependency solver if route has dependencies.
        # If a generator dependency is detected, fall back from fast path
        # (generator deps need AsyncExitStack which C++ fast path doesn't support).
        self._dep_solver = None
        self._param_validator = None
        if self._fast_path_eligible:
            if self.dependant.dependencies:
                try:
                    self._dep_solver = _make_dep_solver(self.dependant, self.dependency_overrides_provider)
                except ValueError:
                    self._fast_path_eligible = False
        elif self.dependant.dependencies:
            # Non-fast-path routes with deps: build solver for shim to use
            try:
                self._dep_solver = _make_dep_solver(self.dependant, self.dependency_overrides_provider)
            except Exception:
                pass
            # Build param validator for routes with constrained/required params.
            # This is created regardless of whether there are deps.
            try:
                self._param_validator = _make_param_validator(self._flat_dependant)
            except Exception:
                self._param_validator = None
        self.app = request_response(self.get_route_handler())

    def get_route_handler(self) -> Callable[[Request], Coroutine[Any, Any, Response]]:
        return get_request_handler(
            dependant=self.dependant,
            body_field=self.body_field,
            status_code=self.status_code,
            response_class=self.response_class,
            response_field=self.response_field,
            response_model_include=self.response_model_include,
            response_model_exclude=self.response_model_exclude,
            response_model_by_alias=self.response_model_by_alias,
            response_model_exclude_unset=self.response_model_exclude_unset,
            batch_specs=self._batch_specs,
            response_model_exclude_defaults=self.response_model_exclude_defaults,
            response_model_exclude_none=self.response_model_exclude_none,
            dependency_overrides_provider=self.dependency_overrides_provider,
            embed_body_fields=self._embed_body_fields,
            core_route_id=self._core_route_id,
        )

    def matches(self, scope: Scope) -> tuple[Match, Scope]:
        match, child_scope = super().matches(scope)
        if match != Match.NONE:
            child_scope["route"] = self
        return match, child_scope


class APIRouter(routing.Router):
    """
    `APIRouter` class, used to group *path operations*, for example to structure
    an app in multiple files. It would then be included in the `FastAPI` app, or
    in another `APIRouter` (ultimately included in the app).

    Read more about it in the
    [FastAPI docs for Bigger Applications - Multiple Files](https://fastapi.tiangolo.com/tutorial/bigger-applications/).

    ## Example

    ```python
    from fastapi import APIRouter, FastAPI

    app = FastAPI()
    router = APIRouter()


    @router.get("/users/", tags=["users"])
    async def read_users():
        return [{"username": "Rick"}, {"username": "Morty"}]


    app.include_router(router)
    ```
    """

    def __init__(
        self,
        *,
        prefix: Annotated[str, Doc("An optional path prefix for the router.")] = "",
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to all the *path operations* in this
                router.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to all the
                *path operations* in this router.

                Read more about it in the
                [FastAPI docs for Bigger Applications - Multiple Files](https://fastapi.tiangolo.com/tutorial/bigger-applications/#include-an-apirouter-with-a-custom-prefix-tags-responses-and-dependencies).
                """
            ),
        ] = None,
        default_response_class: Annotated[
            type[Response],
            Doc(
                """
                The default response class to be used.

                Read more in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#default-response-class).
                """
            ),
        ] = Default(JSONResponse),
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses to be shown in OpenAPI.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Additional Responses in OpenAPI](https://fastapi.tiangolo.com/advanced/additional-responses/).

                And in the
                [FastAPI docs for Bigger Applications](https://fastapi.tiangolo.com/tutorial/bigger-applications/#include-an-apirouter-with-a-custom-prefix-tags-responses-and-dependencies).
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                OpenAPI callbacks that should apply to all *path operations* in this
                router.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        routes: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                **Note**: you probably shouldn't use this parameter, it is inherited
                from Starlette and supported for compatibility.

                ---

                A list of routes to serve incoming HTTP and WebSocket requests.
                """
            ),
            deprecated(
                """
                You normally wouldn't use this parameter with FastAPI, it is inherited
                from Starlette and supported for compatibility.

                In FastAPI, you normally would use the *path operation methods*,
                like `router.get()`, `router.post()`, etc.
                """
            ),
        ] = None,
        redirect_slashes: Annotated[
            bool,
            Doc(
                """
                Whether to detect and redirect slashes in URLs when the client doesn't
                use the same format.
                """
            ),
        ] = True,
        default: Annotated[
            Optional[ASGIApp],
            Doc(
                """
                Default function handler for this router. Used to handle
                404 Not Found errors.
                """
            ),
        ] = None,
        dependency_overrides_provider: Annotated[
            Optional[Any],
            Doc(
                """
                Only used internally by FastAPI to handle dependency overrides.

                You shouldn't need to use it. It normally points to the `FastAPI` app
                object.
                """
            ),
        ] = None,
        route_class: Annotated[
            type[APIRoute],
            Doc(
                """
                Custom route (*path operation*) class to be used by this router.

                Read more about it in the
                [FastAPI docs for Custom Request and APIRoute class](https://fastapi.tiangolo.com/how-to/custom-request-and-route/#custom-apiroute-class-in-a-router).
                """
            ),
        ] = APIRoute,
        on_startup: Annotated[
            Optional[Sequence[Callable[[], Any]]],
            Doc(
                """
                A list of startup event handler functions.

                You should instead use the `lifespan` handlers.

                Read more in the [FastAPI docs for `lifespan`](https://fastapi.tiangolo.com/advanced/events/).
                """
            ),
        ] = None,
        on_shutdown: Annotated[
            Optional[Sequence[Callable[[], Any]]],
            Doc(
                """
                A list of shutdown event handler functions.

                You should instead use the `lifespan` handlers.

                Read more in the
                [FastAPI docs for `lifespan`](https://fastapi.tiangolo.com/advanced/events/).
                """
            ),
        ] = None,
        # the generic to Lifespan[AppType] is the type of the top level application
        # which the router cannot know statically, so we use typing.Any
        lifespan: Annotated[
            Optional[Lifespan[Any]],
            Doc(
                """
                A `Lifespan` context manager handler. This replaces `startup` and
                `shutdown` functions with a single context manager.

                Read more in the
                [FastAPI docs for `lifespan`](https://fastapi.tiangolo.com/advanced/events/).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark all *path operations* in this router as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                To include (or not) all the *path operations* in this router in the
                generated OpenAPI.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> None:
        # Handle on_startup/on_shutdown locally since Starlette removed support
        # Ref: https://github.com/Kludex/starlette/pull/3117
        # TODO: deprecate this once the lifespan (or alternative) interface is improved
        self.on_startup: list[Callable[[], Any]] = (
            [] if on_startup is None else list(on_startup)
        )
        self.on_shutdown: list[Callable[[], Any]] = (
            [] if on_shutdown is None else list(on_shutdown)
        )

        # Determine the lifespan context to use
        if lifespan is None:
            # Use the default lifespan that runs on_startup/on_shutdown handlers
            lifespan_context: Lifespan[Any] = _DefaultLifespan(self)
        elif inspect.isasyncgenfunction(lifespan):
            lifespan_context = asynccontextmanager(lifespan)
        elif inspect.isgeneratorfunction(lifespan):
            lifespan_context = _wrap_gen_lifespan_context(lifespan)
        else:
            lifespan_context = lifespan
        self.lifespan_context = lifespan_context

        super().__init__(
            routes=routes,
            redirect_slashes=redirect_slashes,
            default=default,
            lifespan=lifespan_context,
        )
        if prefix:
            assert prefix.startswith("/"), "A path prefix must start with '/'"
            assert not prefix.endswith("/"), (
                "A path prefix must not end with '/', as the routes will start with '/'"
            )
        self.prefix = prefix
        self.tags: list[Union[str, Enum]] = tags or []
        self.dependencies = list(dependencies or [])
        self.deprecated = deprecated
        self.include_in_schema = include_in_schema
        self.responses = responses or {}
        self.callbacks = callbacks or []
        self.dependency_overrides_provider = dependency_overrides_provider
        self.route_class = route_class
        self.default_response_class = default_response_class
        self.generate_unique_id_function = generate_unique_id_function
        # Core-accelerated trie-based route matching
        self._core_router: Optional[Any] = CoreRouter()
        self._core_route_index: dict[int, APIRoute] = {}

    def route(
        self,
        path: str,
        methods: Optional[Collection[str]] = None,
        name: Optional[str] = None,
        include_in_schema: bool = True,
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        def decorator(func: DecoratedCallable) -> DecoratedCallable:
            self.add_route(
                path,
                func,
                methods=methods,
                name=name,
                include_in_schema=include_in_schema,
            )
            return func

        return decorator

    def add_api_route(
        self,
        path: str,
        endpoint: Callable[..., Any],
        *,
        response_model: Any = Default(None),
        status_code: Optional[int] = None,
        tags: Optional[list[Union[str, Enum]]] = None,
        dependencies: Optional[Sequence[params.Depends]] = None,
        summary: Optional[str] = None,
        description: Optional[str] = None,
        response_description: str = "Successful Response",
        responses: Optional[dict[Union[int, str], dict[str, Any]]] = None,
        deprecated: Optional[bool] = None,
        methods: Optional[Union[set[str], list[str]]] = None,
        operation_id: Optional[str] = None,
        response_model_include: Optional[IncEx] = None,
        response_model_exclude: Optional[IncEx] = None,
        response_model_by_alias: bool = True,
        response_model_exclude_unset: bool = False,
        response_model_exclude_defaults: bool = False,
        response_model_exclude_none: bool = False,
        include_in_schema: bool = True,
        response_class: Union[type[Response], DefaultPlaceholder] = Default(
            JSONResponse
        ),
        name: Optional[str] = None,
        route_class_override: Optional[type[APIRoute]] = None,
        callbacks: Optional[list[BaseRoute]] = None,
        openapi_extra: Optional[dict[str, Any]] = None,
        generate_unique_id_function: Union[
            Callable[[APIRoute], str], DefaultPlaceholder
        ] = Default(generate_unique_id),
    ) -> None:
        route_class = route_class_override or self.route_class
        responses = responses or {}
        combined_responses = {**self.responses, **responses}
        current_response_class = get_value_or_default(
            response_class, self.default_response_class
        )
        current_tags = self.tags.copy()
        if tags:
            current_tags.extend(tags)
        current_dependencies = self.dependencies.copy()
        if dependencies:
            current_dependencies.extend(dependencies)
        current_callbacks = self.callbacks.copy()
        if callbacks:
            current_callbacks.extend(callbacks)
        current_generate_unique_id = get_value_or_default(
            generate_unique_id_function, self.generate_unique_id_function
        )
        route = route_class(
            self.prefix + path,
            endpoint=endpoint,
            response_model=response_model,
            status_code=status_code,
            tags=current_tags,
            dependencies=current_dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=combined_responses,
            deprecated=deprecated or self.deprecated,
            methods=methods,
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema and self.include_in_schema,
            response_class=current_response_class,
            name=name,
            dependency_overrides_provider=self.dependency_overrides_provider,
            callbacks=current_callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=current_generate_unique_id,
        )
        self.routes.append(route)
        # Register route in Core trie for accelerated matching
        if self._core_router is not None:
            try:
                route_idx = len(self.routes) - 1
                self._core_router.add_route(self.prefix + path, route_idx)
                self._core_route_index[route_idx] = route
            except Exception:
                pass  # Fall back to Starlette matching on error

    def api_route(
        self,
        path: str,
        *,
        response_model: Any = Default(None),
        status_code: Optional[int] = None,
        tags: Optional[list[Union[str, Enum]]] = None,
        dependencies: Optional[Sequence[params.Depends]] = None,
        summary: Optional[str] = None,
        description: Optional[str] = None,
        response_description: str = "Successful Response",
        responses: Optional[dict[Union[int, str], dict[str, Any]]] = None,
        deprecated: Optional[bool] = None,
        methods: Optional[list[str]] = None,
        operation_id: Optional[str] = None,
        response_model_include: Optional[IncEx] = None,
        response_model_exclude: Optional[IncEx] = None,
        response_model_by_alias: bool = True,
        response_model_exclude_unset: bool = False,
        response_model_exclude_defaults: bool = False,
        response_model_exclude_none: bool = False,
        include_in_schema: bool = True,
        response_class: type[Response] = Default(JSONResponse),
        name: Optional[str] = None,
        callbacks: Optional[list[BaseRoute]] = None,
        openapi_extra: Optional[dict[str, Any]] = None,
        generate_unique_id_function: Callable[[APIRoute], str] = Default(
            generate_unique_id
        ),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        def decorator(func: DecoratedCallable) -> DecoratedCallable:
            self.add_api_route(
                path,
                func,
                response_model=response_model,
                status_code=status_code,
                tags=tags,
                dependencies=dependencies,
                summary=summary,
                description=description,
                response_description=response_description,
                responses=responses,
                deprecated=deprecated,
                methods=methods,
                operation_id=operation_id,
                response_model_include=response_model_include,
                response_model_exclude=response_model_exclude,
                response_model_by_alias=response_model_by_alias,
                response_model_exclude_unset=response_model_exclude_unset,
                response_model_exclude_defaults=response_model_exclude_defaults,
                response_model_exclude_none=response_model_exclude_none,
                include_in_schema=include_in_schema,
                response_class=response_class,
                name=name,
                callbacks=callbacks,
                openapi_extra=openapi_extra,
                generate_unique_id_function=generate_unique_id_function,
            )
            return func

        return decorator

    def add_api_websocket_route(
        self,
        path: str,
        endpoint: Callable[..., Any],
        name: Optional[str] = None,
        *,
        dependencies: Optional[Sequence[params.Depends]] = None,
    ) -> None:
        current_dependencies = self.dependencies.copy()
        if dependencies:
            current_dependencies.extend(dependencies)

        route = APIWebSocketRoute(
            self.prefix + path,
            endpoint=endpoint,
            name=name,
            dependencies=current_dependencies,
            dependency_overrides_provider=self.dependency_overrides_provider,
        )
        self.routes.append(route)

    def websocket(
        self,
        path: Annotated[
            str,
            Doc(
                """
                WebSocket path.
                """
            ),
        ],
        name: Annotated[
            Optional[str],
            Doc(
                """
                A name for the WebSocket. Only used internally.
                """
            ),
        ] = None,
        *,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be used for this
                WebSocket.

                Read more about it in the
                [FastAPI docs for WebSockets](https://fastapi.tiangolo.com/advanced/websockets/).
                """
            ),
        ] = None,
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Decorate a WebSocket function.

        Read more about it in the
        [FastAPI docs for WebSockets](https://fastapi.tiangolo.com/advanced/websockets/).

        **Example**

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI, WebSocket

        app = FastAPI()
        router = APIRouter()

        @router.websocket("/ws")
        async def websocket_endpoint(websocket: WebSocket):
            await websocket.accept()
            while True:
                data = await websocket.receive_text()
                await websocket.send_text(f"Message text was: {data}")

        app.include_router(router)
        ```
        """

        def decorator(func: DecoratedCallable) -> DecoratedCallable:
            self.add_api_websocket_route(
                path, func, name=name, dependencies=dependencies
            )
            return func

        return decorator

    def websocket_route(
        self, path: str, name: Union[str, None] = None
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        def decorator(func: DecoratedCallable) -> DecoratedCallable:
            self.add_websocket_route(path, func, name=name)
            return func

        return decorator

    def include_router(
        self,
        router: Annotated["APIRouter", Doc("The `APIRouter` to include.")],
        *,
        prefix: Annotated[str, Doc("An optional path prefix for the router.")] = "",
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to all the *path operations* in this
                router.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to all the
                *path operations* in this router.

                Read more about it in the
                [FastAPI docs for Bigger Applications - Multiple Files](https://fastapi.tiangolo.com/tutorial/bigger-applications/#include-an-apirouter-with-a-custom-prefix-tags-responses-and-dependencies).
                """
            ),
        ] = None,
        default_response_class: Annotated[
            type[Response],
            Doc(
                """
                The default response class to be used.

                Read more in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#default-response-class).
                """
            ),
        ] = Default(JSONResponse),
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses to be shown in OpenAPI.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Additional Responses in OpenAPI](https://fastapi.tiangolo.com/advanced/additional-responses/).

                And in the
                [FastAPI docs for Bigger Applications](https://fastapi.tiangolo.com/tutorial/bigger-applications/#include-an-apirouter-with-a-custom-prefix-tags-responses-and-dependencies).
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                OpenAPI callbacks that should apply to all *path operations* in this
                router.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark all *path operations* in this router as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include (or not) all the *path operations* in this router in the
                generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = True,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> None:
        """
        Include another `APIRouter` in the same current `APIRouter`.

        Read more about it in the
        [FastAPI docs for Bigger Applications](https://fastapi.tiangolo.com/tutorial/bigger-applications/).

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI

        app = FastAPI()
        internal_router = APIRouter()
        users_router = APIRouter()

        @users_router.get("/users/")
        def read_users():
            return [{"name": "Rick"}, {"name": "Morty"}]

        internal_router.include_router(users_router)
        app.include_router(internal_router)
        ```
        """
        if prefix:
            assert prefix.startswith("/"), "A path prefix must start with '/'"
            assert not prefix.endswith("/"), (
                "A path prefix must not end with '/', as the routes will start with '/'"
            )
        else:
            for r in router.routes:
                path = getattr(r, "path")  # noqa: B009
                name = getattr(r, "name", "unknown")
                if path is not None and not path:
                    raise FastAPIError(
                        f"Prefix and path cannot be both empty (path operation: {name})"
                    )
        if responses is None:
            responses = {}
        for route in router.routes:
            if isinstance(route, APIRoute):
                combined_responses = {**responses, **route.responses}
                use_response_class = get_value_or_default(
                    route.response_class,
                    router.default_response_class,
                    default_response_class,
                    self.default_response_class,
                )
                current_tags = []
                if tags:
                    current_tags.extend(tags)
                if route.tags:
                    current_tags.extend(route.tags)
                current_dependencies: list[params.Depends] = []
                if dependencies:
                    current_dependencies.extend(dependencies)
                if route.dependencies:
                    current_dependencies.extend(route.dependencies)
                current_callbacks = []
                if callbacks:
                    current_callbacks.extend(callbacks)
                if route.callbacks:
                    current_callbacks.extend(route.callbacks)
                current_generate_unique_id = get_value_or_default(
                    route.generate_unique_id_function,
                    router.generate_unique_id_function,
                    generate_unique_id_function,
                    self.generate_unique_id_function,
                )
                self.add_api_route(
                    prefix + route.path,
                    route.endpoint,
                    response_model=route.response_model,
                    status_code=route.status_code,
                    tags=current_tags,
                    dependencies=current_dependencies,
                    summary=route.summary,
                    description=route.description,
                    response_description=route.response_description,
                    responses=combined_responses,
                    deprecated=route.deprecated or deprecated or self.deprecated,
                    methods=route.methods,
                    operation_id=route.operation_id,
                    response_model_include=route.response_model_include,
                    response_model_exclude=route.response_model_exclude,
                    response_model_by_alias=route.response_model_by_alias,
                    response_model_exclude_unset=route.response_model_exclude_unset,
                    response_model_exclude_defaults=route.response_model_exclude_defaults,
                    response_model_exclude_none=route.response_model_exclude_none,
                    include_in_schema=route.include_in_schema
                    and self.include_in_schema
                    and include_in_schema,
                    response_class=use_response_class,
                    name=route.name,
                    route_class_override=type(route),
                    callbacks=current_callbacks,
                    openapi_extra=route.openapi_extra,
                    generate_unique_id_function=current_generate_unique_id,
                )
            elif isinstance(route, routing.Route):
                methods = list(route.methods or [])
                self.add_route(
                    prefix + route.path,
                    route.endpoint,
                    methods=methods,
                    include_in_schema=route.include_in_schema,
                    name=route.name,
                )
            elif isinstance(route, APIWebSocketRoute):
                current_dependencies = []
                if dependencies:
                    current_dependencies.extend(dependencies)
                if route.dependencies:
                    current_dependencies.extend(route.dependencies)
                self.add_api_websocket_route(
                    prefix + route.path,
                    route.endpoint,
                    dependencies=current_dependencies,
                    name=route.name,
                )
            elif isinstance(route, routing.WebSocketRoute):
                self.add_websocket_route(
                    prefix + route.path, route.endpoint, name=route.name
                )
        for handler in router.on_startup:
            self.add_event_handler("startup", handler)
        for handler in router.on_shutdown:
            self.add_event_handler("shutdown", handler)
        self.lifespan_context = _merge_lifespan_context(
            self.lifespan_context,
            router.lifespan_context,
        )

    async def _core_lookup_route(
        self, scope: Scope
    ) -> Optional[tuple[Match, Scope]]:
        """
        Use the Core trie-based router for O(1) route matching instead of
        Starlette's O(n) sequential regex matching. Returns None if Core
        router is not available or the route is not an API route (e.g., Mount).
        """
        if self._core_router is None:
            return None
        path = scope.get("path", "")
        method = scope.get("method", "GET").upper()
        result = self._core_router.match_route(path)
        if result is None:
            return None
        route_idx, path_params = result
        route = self._core_route_index.get(route_idx)
        if route is None:
            return None
        # Check HTTP method
        if hasattr(route, "methods") and method not in route.methods:
            return None
        # Build child scope matching what Starlette expects
        child_scope: Scope = {
            "type": scope.get("type", "http"),
            "path_params": {**scope.get("path_params", {}), **path_params},
            "endpoint": route.endpoint,
            "app": route.app,
            "route": route,
        }
        return Match.FULL, child_scope

    def get(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP GET operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI

        app = FastAPI()
        router = APIRouter()

        @router.get("/items/")
        def read_items():
            return [{"name": "Empanada"}, {"name": "Arepa"}]

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["GET"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def put(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP PUT operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI
        from pydantic import BaseModel

        class Item(BaseModel):
            name: str
            description: str | None = None

        app = FastAPI()
        router = APIRouter()

        @router.put("/items/{item_id}")
        def replace_item(item_id: str, item: Item):
            return {"message": "Item replaced", "id": item_id}

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["PUT"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def post(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP POST operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI
        from pydantic import BaseModel

        class Item(BaseModel):
            name: str
            description: str | None = None

        app = FastAPI()
        router = APIRouter()

        @router.post("/items/")
        def create_item(item: Item):
            return {"message": "Item created"}

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["POST"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def delete(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP DELETE operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI

        app = FastAPI()
        router = APIRouter()

        @router.delete("/items/{item_id}")
        def delete_item(item_id: str):
            return {"message": "Item deleted"}

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["DELETE"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def options(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP OPTIONS operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI

        app = FastAPI()
        router = APIRouter()

        @router.options("/items/")
        def get_item_options():
            return {"additions": ["Aji", "Guacamole"]}

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["OPTIONS"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def head(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP HEAD operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI
        from pydantic import BaseModel

        class Item(BaseModel):
            name: str
            description: str | None = None

        app = FastAPI()
        router = APIRouter()

        @router.head("/items/", status_code=204)
        def get_items_headers(response: Response):
            response.headers["X-Cat-Dog"] = "Alone in the world"

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["HEAD"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def patch(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP PATCH operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI
        from pydantic import BaseModel

        class Item(BaseModel):
            name: str
            description: str | None = None

        app = FastAPI()
        router = APIRouter()

        @router.patch("/items/")
        def update_item(item: Item):
            return {"message": "Item updated in place"}

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["PATCH"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    def trace(
        self,
        path: Annotated[
            str,
            Doc(
                """
                The URL path to be used for this *path operation*.

                For example, in `http://example.com/items`, the path is `/items`.
                """
            ),
        ],
        *,
        response_model: Annotated[
            Any,
            Doc(
                """
                The type to use for the response.

                It could be any valid Pydantic *field* type. So, it doesn't have to
                be a Pydantic model, it could be other things, like a `list`, `dict`,
                etc.

                It will be used for:

                * Documentation: the generated OpenAPI (and the UI at `/docs`) will
                    show it as the response (JSON Schema).
                * Serialization: you could return an arbitrary object and the
                    `response_model` would be used to serialize that object into the
                    corresponding JSON.
                * Filtering: the JSON sent to the client will only contain the data
                    (fields) defined in the `response_model`. If you returned an object
                    that contains an attribute `password` but the `response_model` does
                    not include that field, the JSON sent to the client would not have
                    that `password`.
                * Validation: whatever you return will be serialized with the
                    `response_model`, converting any data as necessary to generate the
                    corresponding JSON. But if the data in the object returned is not
                    valid, that would mean a violation of the contract with the client,
                    so it's an error from the API developer. So, FastAPI will raise an
                    error and return a 500 error code (Internal Server Error).

                Read more about it in the
                [FastAPI docs for Response Model](https://fastapi.tiangolo.com/tutorial/response-model/).
                """
            ),
        ] = Default(None),
        status_code: Annotated[
            Optional[int],
            Doc(
                """
                The default status code to be used for the response.

                You could override the status code by returning a response directly.

                Read more about it in the
                [FastAPI docs for Response Status Code](https://fastapi.tiangolo.com/tutorial/response-status-code/).
                """
            ),
        ] = None,
        tags: Annotated[
            Optional[list[Union[str, Enum]]],
            Doc(
                """
                A list of tags to be applied to the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/#tags).
                """
            ),
        ] = None,
        dependencies: Annotated[
            Optional[Sequence[params.Depends]],
            Doc(
                """
                A list of dependencies (using `Depends()`) to be applied to the
                *path operation*.

                Read more about it in the
                [FastAPI docs for Dependencies in path operation decorators](https://fastapi.tiangolo.com/tutorial/dependencies/dependencies-in-path-operation-decorators/).
                """
            ),
        ] = None,
        summary: Annotated[
            Optional[str],
            Doc(
                """
                A summary for the *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        description: Annotated[
            Optional[str],
            Doc(
                """
                A description for the *path operation*.

                If not provided, it will be extracted automatically from the docstring
                of the *path operation function*.

                It can contain Markdown.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Path Operation Configuration](https://fastapi.tiangolo.com/tutorial/path-operation-configuration/).
                """
            ),
        ] = None,
        response_description: Annotated[
            str,
            Doc(
                """
                The description for the default response.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = "Successful Response",
        responses: Annotated[
            Optional[dict[Union[int, str], dict[str, Any]]],
            Doc(
                """
                Additional responses that could be returned by this *path operation*.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        deprecated: Annotated[
            Optional[bool],
            Doc(
                """
                Mark this *path operation* as deprecated.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).
                """
            ),
        ] = None,
        operation_id: Annotated[
            Optional[str],
            Doc(
                """
                Custom operation ID to be used by this *path operation*.

                By default, it is generated automatically.

                If you provide a custom operation ID, you need to make sure it is
                unique for the whole API.

                You can customize the
                operation ID generation with the parameter
                `generate_unique_id_function` in the `FastAPI` class.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = None,
        response_model_include: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to include only certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_exclude: Annotated[
            Optional[IncEx],
            Doc(
                """
                Configuration passed to Pydantic to exclude certain fields in the
                response data.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = None,
        response_model_by_alias: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response model
                should be serialized by alias when an alias is used.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_include-and-response_model_exclude).
                """
            ),
        ] = True,
        response_model_exclude_unset: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that were not set and
                have their default values. This is different from
                `response_model_exclude_defaults` in that if the fields are set,
                they will be included in the response, even if the value is the same
                as the default.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_defaults: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data
                should have all the fields, including the ones that have the same value
                as the default. This is different from `response_model_exclude_unset`
                in that if the fields are set but contain the same default values,
                they will be excluded from the response.

                When `True`, default values are omitted from the response.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#use-the-response_model_exclude_unset-parameter).
                """
            ),
        ] = False,
        response_model_exclude_none: Annotated[
            bool,
            Doc(
                """
                Configuration passed to Pydantic to define if the response data should
                exclude fields set to `None`.

                This is much simpler (less smart) than `response_model_exclude_unset`
                and `response_model_exclude_defaults`. You probably want to use one of
                those two instead of this one, as those allow returning `None` values
                when it makes sense.

                Read more about it in the
                [FastAPI docs for Response Model - Return Type](https://fastapi.tiangolo.com/tutorial/response-model/#response_model_exclude_none).
                """
            ),
        ] = False,
        include_in_schema: Annotated[
            bool,
            Doc(
                """
                Include this *path operation* in the generated OpenAPI schema.

                This affects the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for Query Parameters and String Validations](https://fastapi.tiangolo.com/tutorial/query-params-str-validations/#exclude-parameters-from-openapi).
                """
            ),
        ] = True,
        response_class: Annotated[
            type[Response],
            Doc(
                """
                Response class to be used for this *path operation*.

                This will not be used if you return a response directly.

                Read more about it in the
                [FastAPI docs for Custom Response - HTML, Stream, File, others](https://fastapi.tiangolo.com/advanced/custom-response/#redirectresponse).
                """
            ),
        ] = Default(JSONResponse),
        name: Annotated[
            Optional[str],
            Doc(
                """
                Name for this *path operation*. Only used internally.
                """
            ),
        ] = None,
        callbacks: Annotated[
            Optional[list[BaseRoute]],
            Doc(
                """
                List of *path operations* that will be used as OpenAPI callbacks.

                This is only for OpenAPI documentation, the callbacks won't be used
                directly.

                It will be added to the generated OpenAPI (e.g. visible at `/docs`).

                Read more about it in the
                [FastAPI docs for OpenAPI Callbacks](https://fastapi.tiangolo.com/advanced/openapi-callbacks/).
                """
            ),
        ] = None,
        openapi_extra: Annotated[
            Optional[dict[str, Any]],
            Doc(
                """
                Extra metadata to be included in the OpenAPI schema for this *path
                operation*.

                Read more about it in the
                [FastAPI docs for Path Operation Advanced Configuration](https://fastapi.tiangolo.com/advanced/path-operation-advanced-configuration/#custom-openapi-path-operation-schema).
                """
            ),
        ] = None,
        generate_unique_id_function: Annotated[
            Callable[[APIRoute], str],
            Doc(
                """
                Customize the function used to generate unique IDs for the *path
                operations* shown in the generated OpenAPI.

                This is particularly useful when automatically generating clients or
                SDKs for your API.

                Read more about it in the
                [FastAPI docs about how to Generate Clients](https://fastapi.tiangolo.com/advanced/generate-clients/#custom-generate-unique-id-function).
                """
            ),
        ] = Default(generate_unique_id),
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add a *path operation* using an HTTP TRACE operation.

        ## Example

        ```python
        from fastapi import APIRouter, FastAPI
        from pydantic import BaseModel

        class Item(BaseModel):
            name: str
            description: str | None = None

        app = FastAPI()
        router = APIRouter()

        @router.trace("/items/{item_id}")
        def trace_item(item_id: str):
            return None

        app.include_router(router)
        ```
        """
        return self.api_route(
            path=path,
            response_model=response_model,
            status_code=status_code,
            tags=tags,
            dependencies=dependencies,
            summary=summary,
            description=description,
            response_description=response_description,
            responses=responses,
            deprecated=deprecated,
            methods=["TRACE"],
            operation_id=operation_id,
            response_model_include=response_model_include,
            response_model_exclude=response_model_exclude,
            response_model_by_alias=response_model_by_alias,
            response_model_exclude_unset=response_model_exclude_unset,
            response_model_exclude_defaults=response_model_exclude_defaults,
            response_model_exclude_none=response_model_exclude_none,
            include_in_schema=include_in_schema,
            response_class=response_class,
            name=name,
            callbacks=callbacks,
            openapi_extra=openapi_extra,
            generate_unique_id_function=generate_unique_id_function,
        )

    # TODO: remove this once the lifespan (or alternative) interface is improved
    async def _startup(self) -> None:
        """
        Run any `.on_startup` event handlers.

        This method is kept for backward compatibility after Starlette removed
        support for on_startup/on_shutdown handlers.

        Ref: https://github.com/Kludex/starlette/pull/3117
        """
        for handler in self.on_startup:
            if is_async_callable(handler):
                await handler()
            else:
                handler()

    # TODO: remove this once the lifespan (or alternative) interface is improved
    async def _shutdown(self) -> None:
        """
        Run any `.on_shutdown` event handlers.

        This method is kept for backward compatibility after Starlette removed
        support for on_startup/on_shutdown handlers.

        Ref: https://github.com/Kludex/starlette/pull/3117
        """
        for handler in self.on_shutdown:
            if is_async_callable(handler):
                await handler()
            else:
                handler()

    # TODO: remove this once the lifespan (or alternative) interface is improved
    def add_event_handler(
        self,
        event_type: str,
        func: Callable[[], Any],
    ) -> None:
        """
        Add an event handler function for startup or shutdown.

        This method is kept for backward compatibility after Starlette removed
        support for on_startup/on_shutdown handlers.

        Ref: https://github.com/Kludex/starlette/pull/3117
        """
        assert event_type in ("startup", "shutdown")
        if event_type == "startup":
            self.on_startup.append(func)
        else:
            self.on_shutdown.append(func)

    @deprecated(
        """
        on_event is deprecated, use lifespan event handlers instead.

        Read more about it in the
        [FastAPI docs for Lifespan Events](https://fastapi.tiangolo.com/advanced/events/).
        """
    )
    def on_event(
        self,
        event_type: Annotated[
            str,
            Doc(
                """
                The type of event. `startup` or `shutdown`.
                """
            ),
        ],
    ) -> Callable[[DecoratedCallable], DecoratedCallable]:
        """
        Add an event handler for the router.

        `on_event` is deprecated, use `lifespan` event handlers instead.

        Read more about it in the
        [FastAPI docs for Lifespan Events](https://fastapi.tiangolo.com/advanced/events/#alternative-events-deprecated).
        """

        def decorator(func: DecoratedCallable) -> DecoratedCallable:
            self.add_event_handler(event_type, func)
            return func

        return decorator
