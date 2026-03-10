import dataclasses
import inspect
import sys
from collections.abc import Coroutine, Mapping, Sequence
from contextlib import AsyncExitStack, contextmanager
from copy import copy
from dataclasses import dataclass
from typing import (
    Annotated,
    Any,
    Callable,
    ForwardRef,
    Optional,
    Union,
    cast,
)

import anyio
from fastapi import params
from fastapi._compat import (
    ModelField,
    RequiredParam,
    Undefined,
    copy_field_info,
    create_body_model,
    evaluate_forwardref,
    field_annotation_is_scalar,
    get_cached_model_fields,
    get_missing_field_error,
    is_bytes_field,
    is_bytes_sequence_field,
    is_scalar_field,
    is_scalar_sequence_field,
    is_sequence_field,
    is_uploadfile_or_nonable_uploadfile_annotation,
    is_uploadfile_sequence_annotation,
    lenient_issubclass,
    sequence_types,
    serialize_sequence_value,
    value_is_sequence,
)
from fastapi.background import BackgroundTasks
from fastapi.concurrency import (
    asynccontextmanager,
    contextmanager_in_threadpool,
)
from fastapi.dependencies.models import Dependant
from fastapi.exceptions import DependencyScopeError
from fastapi.logger import logger
from fastapi.security.oauth2 import SecurityScopes
from fastapi.types import DependencyCacheKey
from fastapi.utils import create_model_field, get_path_param_names
from pydantic import BaseModel, Json
from pydantic.fields import FieldInfo
from fastapi._background import BackgroundTasks as NativeBackgroundTasks
from fastapi._concurrency import run_in_threadpool
from fastapi._datastructures_impl import (
    FormData,
    Headers,
    ImmutableMultiDict,
    QueryParams,
    UploadFile,
)
from fastapi._request import HTTPConnection, Request
from fastapi._response import Response
from fastapi._websocket import WebSocket
from typing_extensions import Literal, get_args, get_origin
from typing_inspection.typing_objects import is_typealiastype

from fastapi._core_bridge import (
    parse_query_string,
    parse_cookie_header,
    parse_scope_headers,
    batch_coerce_scalars,
    batch_extract_params_inline,
)

multipart_not_installed_error = (
    'Form data requires "python-multipart" to be installed. \n'
    'You can install "python-multipart" with: \n\n'
    "pip install python-multipart\n"
)
multipart_incorrect_install_error = (
    'Form data requires "python-multipart" to be installed. '
    'It seems you installed "multipart" instead. \n'
    'You can remove "multipart" with: \n\n'
    "pip uninstall multipart\n\n"
    'And then install "python-multipart" with: \n\n'
    "pip install python-multipart\n"
)


def ensure_multipart_is_installed() -> None:
    try:
        from python_multipart import __version__

        # Import an attribute that can be mocked/deleted in testing
        assert __version__ > "0.0.12"
    except (ImportError, AssertionError):
        try:
            # __version__ is available in both multiparts, and can be mocked
            from multipart import __version__  # type: ignore[no-redef,import-untyped]

            assert __version__
            try:
                # parse_options_header is only available in the right multipart
                from multipart.multipart import (  # type: ignore[import-untyped]
                    parse_options_header,
                )

                assert parse_options_header
            except ImportError:
                logger.error(multipart_incorrect_install_error)
                raise RuntimeError(multipart_incorrect_install_error) from None
        except ImportError:
            logger.error(multipart_not_installed_error)
            raise RuntimeError(multipart_not_installed_error) from None


def get_parameterless_sub_dependant(*, depends: params.Depends, path: str) -> Dependant:
    assert callable(depends.dependency), (
        "A parameter-less dependency must have a callable dependency"
    )
    own_oauth_scopes: list[str] = []
    if isinstance(depends, params.Security) and depends.scopes:
        own_oauth_scopes.extend(depends.scopes)
    return get_dependant(
        path=path,
        call=depends.dependency,
        scope=depends.scope,
        own_oauth_scopes=own_oauth_scopes,
    )


def get_flat_dependant(
    dependant: Dependant,
    *,
    skip_repeats: bool = False,
    visited: Optional[set[DependencyCacheKey]] = None,
    parent_oauth_scopes: Optional[list[str]] = None,
) -> Dependant:
    if visited is None:
        visited = set()
    visited.add(dependant.cache_key)
    use_parent_oauth_scopes = (parent_oauth_scopes or []) + (
        dependant.oauth_scopes or []
    )

    flat_dependant = Dependant(
        path_params=dependant.path_params.copy(),
        query_params=dependant.query_params.copy(),
        header_params=dependant.header_params.copy(),
        cookie_params=dependant.cookie_params.copy(),
        body_params=dependant.body_params.copy(),
        name=dependant.name,
        call=dependant.call,
        request_param_name=dependant.request_param_name,
        websocket_param_name=dependant.websocket_param_name,
        http_connection_param_name=dependant.http_connection_param_name,
        response_param_name=dependant.response_param_name,
        background_tasks_param_name=dependant.background_tasks_param_name,
        security_scopes_param_name=dependant.security_scopes_param_name,
        own_oauth_scopes=dependant.own_oauth_scopes,
        parent_oauth_scopes=use_parent_oauth_scopes,
        use_cache=dependant.use_cache,
        path=dependant.path,
        scope=dependant.scope,
    )
    for sub_dependant in dependant.dependencies:
        if skip_repeats and sub_dependant.cache_key in visited:
            continue
        flat_sub = get_flat_dependant(
            sub_dependant,
            skip_repeats=skip_repeats,
            visited=visited,
            parent_oauth_scopes=flat_dependant.oauth_scopes,
        )
        flat_dependant.dependencies.append(flat_sub)
        flat_dependant.path_params.extend(flat_sub.path_params)
        flat_dependant.query_params.extend(flat_sub.query_params)
        flat_dependant.header_params.extend(flat_sub.header_params)
        flat_dependant.cookie_params.extend(flat_sub.cookie_params)
        flat_dependant.body_params.extend(flat_sub.body_params)
        flat_dependant.dependencies.extend(flat_sub.dependencies)

    return flat_dependant


def _get_flat_fields_from_params(fields: list[ModelField]) -> list[ModelField]:
    if not fields:
        return fields
    first_field = fields[0]
    if len(fields) == 1 and lenient_issubclass(first_field.type_, BaseModel):
        fields_to_extract = get_cached_model_fields(first_field.type_)
        return fields_to_extract
    return fields


def get_flat_params(dependant: Dependant) -> list[ModelField]:
    flat_dependant = get_flat_dependant(dependant, skip_repeats=True)
    path_params = _get_flat_fields_from_params(flat_dependant.path_params)
    query_params = _get_flat_fields_from_params(flat_dependant.query_params)
    header_params = _get_flat_fields_from_params(flat_dependant.header_params)
    cookie_params = _get_flat_fields_from_params(flat_dependant.cookie_params)
    return path_params + query_params + header_params + cookie_params


def _get_signature(call: Callable[..., Any]) -> inspect.Signature:
    if sys.version_info >= (3, 10):
        try:
            signature = inspect.signature(call, eval_str=True)
        except NameError:
            # Handle type annotations with if TYPE_CHECKING, not used by FastAPI
            # e.g. dependency return types
            if sys.version_info >= (3, 14):
                from annotationlib import Format

                signature = inspect.signature(call, annotation_format=Format.FORWARDREF)
            else:
                signature = inspect.signature(call)
    else:
        signature = inspect.signature(call)
    return signature


def get_typed_signature(call: Callable[..., Any]) -> inspect.Signature:
    signature = _get_signature(call)
    unwrapped = inspect.unwrap(call)
    globalns = getattr(unwrapped, "__globals__", {})
    typed_params = [
        inspect.Parameter(
            name=param.name,
            kind=param.kind,
            default=param.default,
            annotation=get_typed_annotation(param.annotation, globalns),
        )
        for param in signature.parameters.values()
    ]
    typed_signature = inspect.Signature(typed_params)
    return typed_signature


def get_typed_annotation(annotation: Any, globalns: dict[str, Any]) -> Any:
    if isinstance(annotation, str):
        annotation = ForwardRef(annotation)
        annotation = evaluate_forwardref(annotation, globalns, globalns)
        if annotation is type(None):
            return None
    return annotation


def get_typed_return_annotation(call: Callable[..., Any]) -> Any:
    signature = _get_signature(call)
    unwrapped = inspect.unwrap(call)
    annotation = signature.return_annotation

    if annotation is inspect.Signature.empty:
        return None

    globalns = getattr(unwrapped, "__globals__", {})
    return get_typed_annotation(annotation, globalns)


def get_dependant(
    *,
    path: str,
    call: Callable[..., Any],
    name: Optional[str] = None,
    own_oauth_scopes: Optional[list[str]] = None,
    parent_oauth_scopes: Optional[list[str]] = None,
    use_cache: bool = True,
    scope: Union[Literal["function", "request"], None] = None,
) -> Dependant:
    dependant = Dependant(
        call=call,
        name=name,
        path=path,
        use_cache=use_cache,
        scope=scope,
        own_oauth_scopes=own_oauth_scopes,
        parent_oauth_scopes=parent_oauth_scopes,
    )
    current_scopes = (parent_oauth_scopes or []) + (own_oauth_scopes or [])
    path_param_names = get_path_param_names(path)
    endpoint_signature = get_typed_signature(call)
    signature_params = endpoint_signature.parameters
    for param_name, param in signature_params.items():
        is_path_param = param_name in path_param_names
        param_details = analyze_param(
            param_name=param_name,
            annotation=param.annotation,
            value=param.default,
            is_path_param=is_path_param,
        )
        if param_details.depends is not None:
            assert param_details.depends.dependency
            if (
                (dependant.is_gen_callable or dependant.is_async_gen_callable)
                and dependant.computed_scope == "request"
                and param_details.depends.scope == "function"
            ):
                assert dependant.call
                raise DependencyScopeError(
                    f'The dependency "{dependant.call.__name__}" has a scope of '
                    '"request", it cannot depend on dependencies with scope "function".'
                )
            sub_own_oauth_scopes: list[str] = []
            if isinstance(param_details.depends, params.Security):
                if param_details.depends.scopes:
                    sub_own_oauth_scopes = list(param_details.depends.scopes)
            sub_dependant = get_dependant(
                path=path,
                call=param_details.depends.dependency,
                name=param_name,
                own_oauth_scopes=sub_own_oauth_scopes,
                parent_oauth_scopes=current_scopes,
                use_cache=param_details.depends.use_cache,
                scope=param_details.depends.scope,
            )
            dependant.dependencies.append(sub_dependant)
            continue
        if add_non_field_param_to_dependency(
            param_name=param_name,
            type_annotation=param_details.type_annotation,
            dependant=dependant,
        ):
            assert param_details.field is None, (
                f"Cannot specify multiple FastAPI annotations for {param_name!r}"
            )
            continue
        assert param_details.field is not None
        if isinstance(param_details.field.field_info, params.Body):
            dependant.body_params.append(param_details.field)
        else:
            add_param_to_fields(field=param_details.field, dependant=dependant)
    return dependant


def add_non_field_param_to_dependency(
    *, param_name: str, type_annotation: Any, dependant: Dependant
) -> Optional[bool]:
    if lenient_issubclass(type_annotation, Request):
        dependant.request_param_name = param_name
        return True
    elif lenient_issubclass(type_annotation, WebSocket):
        dependant.websocket_param_name = param_name
        return True
    elif lenient_issubclass(type_annotation, HTTPConnection):
        dependant.http_connection_param_name = param_name
        return True
    elif lenient_issubclass(type_annotation, Response):
        dependant.response_param_name = param_name
        return True
    elif lenient_issubclass(type_annotation, NativeBackgroundTasks):
        dependant.background_tasks_param_name = param_name
        return True
    elif lenient_issubclass(type_annotation, SecurityScopes):
        dependant.security_scopes_param_name = param_name
        return True
    return None


@dataclass
class ParamDetails:
    type_annotation: Any
    depends: Optional[params.Depends]
    field: Optional[ModelField]


def analyze_param(
    *,
    param_name: str,
    annotation: Any,
    value: Any,
    is_path_param: bool,
) -> ParamDetails:
    field_info = None
    depends = None
    type_annotation: Any = Any
    use_annotation: Any = Any
    if is_typealiastype(annotation):
        # unpack in case PEP 695 type syntax is used
        annotation = annotation.__value__
    if annotation is not inspect.Signature.empty:
        use_annotation = annotation
        type_annotation = annotation
    # Extract Annotated info
    if get_origin(use_annotation) is Annotated:
        annotated_args = get_args(annotation)
        type_annotation = annotated_args[0]
        fastapi_annotations = [
            arg
            for arg in annotated_args[1:]
            if isinstance(arg, (FieldInfo, params.Depends))
        ]
        fastapi_specific_annotations = [
            arg
            for arg in fastapi_annotations
            if isinstance(
                arg,
                (
                    params.Param,
                    params.Body,
                    params.Depends,
                ),
            )
        ]
        if fastapi_specific_annotations:
            fastapi_annotation: Union[FieldInfo, params.Depends, None] = (
                fastapi_specific_annotations[-1]
            )
        else:
            fastapi_annotation = None
        # Set default for Annotated FieldInfo
        if isinstance(fastapi_annotation, FieldInfo):
            # Copy `field_info` because we mutate `field_info.default` below.
            field_info = copy_field_info(
                field_info=fastapi_annotation,
                annotation=use_annotation,
            )
            assert (
                field_info.default == Undefined or field_info.default == RequiredParam
            ), (
                f"`{field_info.__class__.__name__}` default value cannot be set in"
                f" `Annotated` for {param_name!r}. Set the default value with `=` instead."
            )
            if value is not inspect.Signature.empty:
                assert not is_path_param, "Path parameters cannot have default values"
                field_info.default = value
            else:
                field_info.default = RequiredParam
        # Get Annotated Depends
        elif isinstance(fastapi_annotation, params.Depends):
            depends = fastapi_annotation
    # Get Depends from default value
    if isinstance(value, params.Depends):
        assert depends is None, (
            "Cannot specify `Depends` in `Annotated` and default value"
            f" together for {param_name!r}"
        )
        assert field_info is None, (
            "Cannot specify a FastAPI annotation in `Annotated` and `Depends` as a"
            f" default value together for {param_name!r}"
        )
        depends = value
    # Get FieldInfo from default value
    elif isinstance(value, FieldInfo):
        assert field_info is None, (
            "Cannot specify FastAPI annotations in `Annotated` and default value"
            f" together for {param_name!r}"
        )
        field_info = value
        if isinstance(field_info, FieldInfo):
            field_info.annotation = type_annotation

    # Get Depends from type annotation
    if depends is not None and depends.dependency is None:
        # Copy `depends` before mutating it
        depends = copy(depends)
        depends = dataclasses.replace(depends, dependency=type_annotation)

    # Handle non-param type annotations like Request, WebSocket, etc.
    # These should never be treated as fields, even with Depends
    if lenient_issubclass(
        type_annotation,
        (
            Request,
            WebSocket,
            HTTPConnection,
            Response,
            NativeBackgroundTasks,
            SecurityScopes,
        ),
    ):
        # If there's a Depends, it will be handled separately
        # If there's no Depends, the special injection will be used
        if depends is None:
            assert field_info is None, (
                f"Cannot specify FastAPI annotation for type {type_annotation!r}"
            )
        # Return early - don't create a field for these special types
        return ParamDetails(type_annotation=type_annotation, depends=depends, field=None)
    # Handle default assignations, neither field_info nor depends was not found in Annotated nor default value
    elif field_info is None and depends is None:
        default_value = value if value is not inspect.Signature.empty else RequiredParam
        if is_path_param:
            # We might check here that `default_value is RequiredParam`, but the fact is that the same
            # parameter might sometimes be a path parameter and sometimes not. See
            # `tests/test_infer_param_optionality.py` for an example.
            field_info = params.Path(annotation=use_annotation)
        elif is_uploadfile_or_nonable_uploadfile_annotation(
            type_annotation
        ) or is_uploadfile_sequence_annotation(type_annotation):
            field_info = params.File(annotation=use_annotation, default=default_value)
        elif not field_annotation_is_scalar(annotation=type_annotation):
            field_info = params.Body(annotation=use_annotation, default=default_value)
        else:
            field_info = params.Query(annotation=use_annotation, default=default_value)

    field = None
    # It's a field_info, not a dependency
    if field_info is not None:
        # Handle field_info.in_
        if is_path_param:
            assert isinstance(field_info, params.Path), (
                f"Cannot use `{field_info.__class__.__name__}` for path param"
                f" {param_name!r}"
            )
        elif (
            isinstance(field_info, params.Param)
            and getattr(field_info, "in_", None) is None
        ):
            field_info.in_ = params.ParamTypes.query
        use_annotation_from_field_info = use_annotation
        if isinstance(field_info, params.Form):
            ensure_multipart_is_installed()
        if not field_info.alias and getattr(field_info, "convert_underscores", None):
            alias = param_name.replace("_", "-")
        else:
            alias = field_info.alias or param_name
        field_info.alias = alias
        field = create_model_field(
            name=param_name,
            type_=use_annotation_from_field_info,
            default=field_info.default,
            alias=alias,
            required=field_info.default in (RequiredParam, Undefined),
            field_info=field_info,
        )
        if is_path_param:
            assert is_scalar_field(field=field), (
                "Path params must be of one of the supported types"
            )
        elif isinstance(field_info, params.Query):
            assert (
                is_scalar_field(field)
                or is_scalar_sequence_field(field)
                or (
                    lenient_issubclass(field.type_, BaseModel)
                    # For Pydantic v1
                    and getattr(field, "shape", 1) == 1
                )
            ), f"Query parameter {param_name!r} must be one of the supported types"

    return ParamDetails(type_annotation=type_annotation, depends=depends, field=field)


def add_param_to_fields(*, field: ModelField, dependant: Dependant) -> None:
    field_info = field.field_info
    field_info_in = getattr(field_info, "in_", None)
    if field_info_in == params.ParamTypes.path:
        dependant.path_params.append(field)
    elif field_info_in == params.ParamTypes.query:
        dependant.query_params.append(field)
    elif field_info_in == params.ParamTypes.header:
        dependant.header_params.append(field)
    else:
        assert field_info_in == params.ParamTypes.cookie, (
            f"non-body parameters must be in path, query, header or cookie: {field.name}"
        )
        dependant.cookie_params.append(field)


async def _solve_generator(
    *, dependant: Dependant, stack: AsyncExitStack, sub_values: dict[str, Any]
) -> Any:
    assert dependant.call
    if dependant.is_async_gen_callable:
        cm = asynccontextmanager(dependant.call)(**sub_values)
    elif dependant.is_gen_callable:
        cm = contextmanager_in_threadpool(contextmanager(dependant.call)(**sub_values))
    return await stack.enter_async_context(cm)


@dataclass
class SolvedDependency:
    values: dict[str, Any]
    errors: list[Any]
    background_tasks: Optional[NativeBackgroundTasks]
    response: Response
    dependency_cache: dict[DependencyCacheKey, Any]


async def solve_dependencies(
    *,
    request: Union[Request, WebSocket],
    dependant: Dependant,
    body: Optional[Union[dict[str, Any], FormData]] = None,
    background_tasks: Optional[NativeBackgroundTasks] = None,
    response: Optional[Response] = None,
    dependency_overrides_provider: Optional[Any] = None,
    dependency_cache: Optional[dict[DependencyCacheKey, Any]] = None,
    # TODO: remove this parameter later, no longer used, not removing it yet as some
    # people might be monkey patching this function (although that's not supported)
    async_exit_stack: AsyncExitStack,
    embed_body_fields: bool,
) -> SolvedDependency:
    # Fast exit for endpoints with no params and no deps (e.g. simple() -> dict)
    if (
        not dependant.dependencies
        and not dependant.path_params
        and not dependant.query_params
        and not dependant.header_params
        and not dependant.cookie_params
        and not dependant.body_params
        and not dependant.request_param_name
        and not dependant.websocket_param_name
        and not dependant.http_connection_param_name
        and not dependant.response_param_name
        and not dependant.background_tasks_param_name
        and not dependant.security_scopes_param_name
    ):
        if response is None:
            response = Response()
            del response.headers["content-length"]
            response.status_code = None  # type: ignore
        return SolvedDependency(
            values={},
            errors=[],
            background_tasks=background_tasks,
            response=response,
            dependency_cache=dependency_cache or {},
        )
    request_astack = request.scope.get("fastapi_inner_astack")
    assert isinstance(request_astack, AsyncExitStack), (
        "fastapi_inner_astack not found in request scope"
    )
    function_astack = request.scope.get("fastapi_function_astack")
    assert isinstance(function_astack, AsyncExitStack), (
        "fastapi_function_astack not found in request scope"
    )
    values: dict[str, Any] = {}
    errors: list[Any] = []
    if response is None:
        response = Response()
        del response.headers["content-length"]
        response.status_code = None  # type: ignore
    if dependency_cache is None:
        dependency_cache = {}
    for sub_dependant in dependant.dependencies:
        sub_dependant.call = cast(Callable[..., Any], sub_dependant.call)
        call = sub_dependant.call
        use_sub_dependant = sub_dependant
        if (
            dependency_overrides_provider
            and dependency_overrides_provider.dependency_overrides
        ):
            original_call = sub_dependant.call
            call = getattr(
                dependency_overrides_provider, "dependency_overrides", {}
            ).get(original_call, original_call)
            use_path: str = sub_dependant.path  # type: ignore
            use_sub_dependant = get_dependant(
                path=use_path,
                call=call,
                name=sub_dependant.name,
                parent_oauth_scopes=sub_dependant.oauth_scopes,
                scope=sub_dependant.scope,
            )

        solved_result = await solve_dependencies(
            request=request,
            dependant=use_sub_dependant,
            body=body,
            background_tasks=background_tasks,
            response=response,
            dependency_overrides_provider=dependency_overrides_provider,
            dependency_cache=dependency_cache,
            async_exit_stack=async_exit_stack,
            embed_body_fields=embed_body_fields,
        )
        background_tasks = solved_result.background_tasks
        if solved_result.errors:
            errors.extend(solved_result.errors)
            continue
        if sub_dependant.use_cache and sub_dependant.cache_key in dependency_cache:
            solved = dependency_cache[sub_dependant.cache_key]
        elif (
            use_sub_dependant.is_gen_callable or use_sub_dependant.is_async_gen_callable
        ):
            use_astack = request_astack
            if sub_dependant.scope == "function":
                use_astack = function_astack
            solved = await _solve_generator(
                dependant=use_sub_dependant,
                stack=use_astack,
                sub_values=solved_result.values,
            )
        elif use_sub_dependant.is_coroutine_callable:
            solved = await call(**solved_result.values)
        else:
            solved = await run_in_threadpool(call, **solved_result.values)
        if sub_dependant.name is not None:
            values[sub_dependant.name] = solved
        if sub_dependant.use_cache and sub_dependant.cache_key not in dependency_cache:
            dependency_cache[sub_dependant.cache_key] = solved
    # v1.0: Batch extract all params (path+query+header+cookie) in one core call
    _batch_ok = False
    _has_model_header = (
        len(dependant.header_params) == 1
        and lenient_issubclass(dependant.header_params[0].type_, BaseModel)
    )
    _has_sequence_header = any(
        is_sequence_field(f) and not _is_json_field(f)
        for f in dependant.header_params
    )
    if (
        isinstance(request, (Request, WebSocket))
        and not _has_model_header
        and not _has_sequence_header
        and (
            dependant.path_params
            or dependant.query_params
            or dependant.header_params
            or dependant.cookie_params
        )
    ):
        try:
            # Use pre-computed batch specs from scope (cached at route registration)
            _specs = request.scope.get("_batch_specs")
            if _specs is None:
                _specs = []
                for field in dependant.query_params:
                    alias = get_validation_alias(field)
                    tag_str = _get_scalar_type_tag(field) or ""
                    tag_int = {"str": 0, "int": 1, "float": 2, "bool": 3}.get(tag_str, 0)
                    _specs.append({
                        "field_name": field.name,
                        "alias": alias,
                        "location": 0,
                        "type_tag": tag_int,
                        "default_value": None,
                        "is_sequence": is_sequence_field(field),
                    })
                for field in dependant.header_params:
                    alias = get_validation_alias(field)
                    tag_str = _get_scalar_type_tag(field) or ""
                    tag_int = {"str": 0, "int": 1, "float": 2, "bool": 3}.get(tag_str, 0)
                    _specs.append({
                        "field_name": field.name,
                        "alias": alias,
                        "header_lookup_key": alias,
                        "location": 1,
                        "type_tag": tag_int,
                        "default_value": None,
                    })
                for field in dependant.cookie_params:
                    alias = get_validation_alias(field)
                    tag_str = _get_scalar_type_tag(field) or ""
                    tag_int = {"str": 0, "int": 1, "float": 2, "bool": 3}.get(tag_str, 0)
                    _specs.append({
                        "field_name": field.name,
                        "alias": alias,
                        "location": 2,
                        "type_tag": tag_int,
                        "default_value": None,
                    })
                for field in dependant.path_params:
                    alias = get_validation_alias(field)
                    tag_str = _get_scalar_type_tag(field) or ""
                    tag_int = {"str": 0, "int": 1, "float": 2, "bool": 3}.get(tag_str, 0)
                    _specs.append({
                        "field_name": field.name,
                        "alias": alias,
                        "location": 3,
                        "type_tag": tag_int,
                        "default_value": None,
                    })

            _qp = request.scope.get("_core_query_params")
            if _qp is None and dependant.query_params:
                _qs = request.scope.get("query_string", b"").decode("latin-1")
                _qp = parse_query_string(_qs)
            if _qp is None:
                _qp = {}
            _hdrs = None
            if dependant.header_params:
                # Parse headers WITHOUT convert_underscores — the core batch
                # extractor handles per-field conversion at lookup time.
                # Don't reuse _core_headers cache (it has conversion applied).
                _hdrs = parse_scope_headers(
                    request.scope.get("headers", []), False
                )
            if _hdrs is None:
                _hdrs = {}
            _cookies = request.scope.get("_core_cookies")
            if _cookies is None and dependant.cookie_params:
                _cs = ""
                for nb, vb in request.scope.get("headers", []):
                    if nb == b"cookie":
                        if _cs:
                            _cs += "; "
                        _cs += vb.decode("latin-1")
                _cookies = parse_cookie_header(_cs)
            if _cookies is None:
                _cookies = {}

            extracted = batch_extract_params_inline(
                _qp, _hdrs, _cookies, request.path_params, _specs
            )

            all_param_fields = (
                list(dependant.path_params)
                + list(dependant.query_params)
                + list(dependant.header_params)
                + list(dependant.cookie_params)
            )
            for field in all_param_fields:
                value = extracted.get(field.name)
                if (
                    value is not None
                    and is_sequence_field(field)
                    and isinstance(value, list)
                    and len(value) == 0
                ):
                    value = None
                field_info = field.field_info
                assert isinstance(field_info, params.Param), (
                    "Params must be subclasses of Param"
                )
                alias = get_validation_alias(field)
                loc = (field_info.in_.value, alias)
                v_, errors_ = _validate_value_with_model_field(
                    field=field, value=value, values=values, loc=loc
                )
                if errors_:
                    errors.extend(errors_)
                else:
                    values[field.name] = v_
            _batch_ok = True
        except Exception:
            _batch_ok = False

    if not _batch_ok:
        path_values, path_errors = request_params_to_args(
            dependant.path_params, request.path_params
        )
        if dependant.query_params and isinstance(
            request, (Request, WebSocket)
        ):
            core_parsed = request.scope.get("_core_query_params")
            if core_parsed is None:
                query_string = request.scope.get("query_string", b"").decode(
                    "latin-1"
                )
                core_parsed = parse_query_string(query_string)
            query_values, query_errors = _core_query_params_to_args(
                dependant.query_params, core_parsed
            )
        else:
            query_values, query_errors = request_params_to_args(
                dependant.query_params, request.query_params
            )
        if (
            dependant.header_params
            and not _has_sequence_header
            and isinstance(request, (Request, WebSocket))
        ):
            core_headers = request.scope.get("_core_headers")
            if core_headers is None:
                raw_hdrs = request.scope.get("headers", [])
                core_headers = parse_scope_headers(raw_hdrs)
            header_values, header_errors = _core_header_params_to_args(
                dependant.header_params, core_headers
            )
        else:
            header_values, header_errors = request_params_to_args(
                dependant.header_params, request.headers
            )
        if dependant.cookie_params and isinstance(
            request, (Request, WebSocket)
        ):
            core_cookies = request.scope.get("_core_cookies")
            if core_cookies is None:
                cookie_str = ""
                for name_bytes, val_bytes in request.scope.get("headers", []):
                    if name_bytes == b"cookie":
                        if cookie_str:
                            cookie_str += "; "
                        cookie_str += val_bytes.decode("latin-1")
                core_cookies = parse_cookie_header(cookie_str)
            cookie_values, cookie_errors = _core_cookie_params_to_args(
                dependant.cookie_params, core_cookies
            )
        else:
            cookie_values, cookie_errors = request_params_to_args(
                dependant.cookie_params, request.cookies
            )
        values.update(path_values)
        values.update(query_values)
        values.update(header_values)
        values.update(cookie_values)
        errors += path_errors + query_errors + header_errors + cookie_errors
    if dependant.body_params:
        (
            body_values,
            body_errors,
        ) = await request_body_to_args(  # body_params checked above
            body_fields=dependant.body_params,
            received_body=body,
            embed_body_fields=embed_body_fields,
        )
        values.update(body_values)
        errors.extend(body_errors)
    if dependant.http_connection_param_name:
        values[dependant.http_connection_param_name] = request
    if dependant.request_param_name and isinstance(request, Request):
        values[dependant.request_param_name] = request
    elif dependant.websocket_param_name and isinstance(request, WebSocket):
        values[dependant.websocket_param_name] = request
    if dependant.background_tasks_param_name:
        if background_tasks is None:
            background_tasks = BackgroundTasks()
        values[dependant.background_tasks_param_name] = background_tasks
    if dependant.response_param_name:
        values[dependant.response_param_name] = response
    if dependant.security_scopes_param_name:
        values[dependant.security_scopes_param_name] = SecurityScopes(
            scopes=dependant.oauth_scopes
        )
    return SolvedDependency(
        values=values,
        errors=errors,
        background_tasks=background_tasks,
        response=response,
        dependency_cache=dependency_cache,
    )


_IMMUTABLE_TYPES = (type(None), str, int, float, bool, tuple, frozenset, bytes)


def _safe_default(val: Any) -> Any:
    """Return field default efficiently: skip copy for immutable types."""
    if isinstance(val, _IMMUTABLE_TYPES):
        return val
    return copy(val)


def _validate_value_with_model_field(
    *, field: ModelField, value: Any, values: dict[str, Any], loc: tuple[str, ...]
) -> tuple[Any, list[Any]]:
    if value is None:
        if field.required:
            return None, [get_missing_field_error(loc=loc)]
        else:
            return _safe_default(field.default), []
    return field.validate(value, values, loc=loc)


def _is_json_field(field: ModelField) -> bool:
    return any(type(item) is Json for item in field.field_info.metadata)


def _get_multidict_value(
    field: ModelField, values: Mapping[str, Any], alias: Union[str, None] = None
) -> Any:
    alias = alias or get_validation_alias(field)
    if (
        (not _is_json_field(field))
        and is_sequence_field(field)
        and isinstance(values, (ImmutableMultiDict, Headers))
    ):
        value = values.getlist(alias)
    else:
        value = values.get(alias, None)
    if (
        value is None
        or (
            isinstance(field.field_info, params.Form)
            and isinstance(value, str)  # For type checks
            and value == ""
        )
        or (is_sequence_field(field) and len(value) == 0)
    ):
        if field.required:
            return
        else:
            return _safe_default(field.default)
    return value


def _get_scalar_type_tag(field: ModelField) -> Optional[str]:
    """Return a type tag for simple scalar types that can be pre-coerced in core."""
    annotation = field.field_info.annotation
    if annotation is int:
        return "int"
    if annotation is float:
        return "float"
    if annotation is bool:
        return "bool"
    if annotation is str:
        return "str"
    return None


def _core_query_params_to_args(
    fields: Sequence[ModelField],
    core_parsed: dict[str, list[str]],
) -> tuple[dict[str, Any], list[Any]]:
    """
    Extract query parameters using core pre-parsed query string.

    The core_parsed dict maps param names to lists of values (multi-value).
    This avoids re-parsing the query string in Python for each field.
    Scalar types (int, float, bool) are batch pre-coerced in core before
    Pydantic validation, reducing per-field Python overhead.
    Handles model-based params (single field with BaseModel type) by delegating
    to request_params_to_args which has the full model-expansion logic.
    """
    values: dict[str, Any] = {}
    errors: list[dict[str, Any]] = []

    if not fields:
        return values, errors

    # If there's a single model-based query param, use request_params_to_args
    # which handles the BaseModel expansion correctly.
    if (
        len(fields) == 1
        and lenient_issubclass(fields[0].type_, BaseModel)
    ):
        from starlette.datastructures import QueryParams as _QP
        flat: dict[str, Any] = {}
        for key, vals in core_parsed.items():
            if vals and len(vals) == 1:
                flat[key] = vals[0]
            elif vals:
                flat[key] = vals
        return request_params_to_args(fields, flat)

    # Build a flat dict for scalar pre-coercion and extract raw values
    raw_values: dict[str, Any] = {}
    field_specs: list[tuple[str, str, str]] = []
    for field in fields:
        alias = get_validation_alias(field)
        field_values = core_parsed.get(alias)
        if field_values is not None:
            if is_sequence_field(field) and not _is_json_field(field):
                raw_values[alias] = field_values
            else:
                val = field_values[0] if field_values else None
                raw_values[alias] = val
                # Only pre-coerce non-sequence scalar fields
                tag = _get_scalar_type_tag(field)
                if tag and val is not None:
                    field_specs.append((alias, tag, field.name))

    # Batch pre-coerce scalars in core
    coerced: dict[str, Any] = {}
    if field_specs:
        try:
            coerced = batch_coerce_scalars(raw_values, field_specs)
        except Exception:
            pass

    for field in fields:
        alias = get_validation_alias(field)
        value = raw_values.get(alias)

        if value is None or (is_sequence_field(field) and len(value) == 0):
            if field.required:
                value = None
            else:
                value = _safe_default(field.default)
                values[field.name] = value
                continue

        # Use pre-coerced value if available
        if field.name in coerced:
            value = coerced[field.name]

        field_info = field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc = (field_info.in_.value, alias)
        v_, errors_ = _validate_value_with_model_field(
            field=field, value=value, values=values, loc=loc
        )
        if errors_:
            errors.extend(errors_)
        else:
            values[field.name] = v_
    return values, errors


def _core_header_params_to_args(
    fields: Sequence[ModelField],
    core_headers: dict[str, str],
) -> tuple[dict[str, Any], list[Any]]:
    """
    Extract header parameters using core pre-parsed headers dict.

    The core_headers dict has lowercased keys with underscores converted to
    dashes (matching Starlette's Header() default behavior).
    For Pydantic model-based headers, builds the full params dict for model
    validation. Falls through to per-field validation otherwise.
    """
    values: dict[str, Any] = {}
    errors: list[dict[str, Any]] = []

    if not fields:
        return values, errors

    first_field = fields[0]
    fields_to_extract = fields
    single_not_embedded_field = False
    default_convert_underscores = True

    if len(fields) == 1 and lenient_issubclass(first_field.type_, BaseModel):
        fields_to_extract = get_cached_model_fields(first_field.type_)
        single_not_embedded_field = True
        default_convert_underscores = getattr(
            first_field.field_info, "convert_underscores", True
        )

    params_to_process: dict[str, Any] = {}
    processed_keys: set[str] = set()

    for field in fields_to_extract:
        convert_underscores = getattr(
            field.field_info, "convert_underscores", default_convert_underscores
        )
        alias = get_validation_alias(field)
        lookup_key = alias
        if convert_underscores and alias == field.name:
            lookup_key = alias.replace("_", "-")
        # core headers are already lowercased
        lookup_key_lower = lookup_key.lower()

        value = core_headers.get(lookup_key_lower)
        if value is not None:
            params_to_process[alias] = value
        processed_keys.add(lookup_key_lower)

    # For Pydantic model headers, include unprocessed headers
    if single_not_embedded_field:
        for key, value in core_headers.items():
            if key not in processed_keys:
                params_to_process[key] = value
        field_info = first_field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc: tuple[str, ...] = (field_info.in_.value,)
        v_, errors_ = _validate_value_with_model_field(
            field=first_field, value=params_to_process, values=values, loc=loc
        )
        return {first_field.name: v_}, errors_

    # Batch pre-coerce scalar header values
    header_field_specs: list[tuple[str, str, str]] = []
    for field in fields:
        tag = _get_scalar_type_tag(field)
        if tag:
            alias = get_validation_alias(field)
            if alias in params_to_process:
                header_field_specs.append((alias, tag, field.name))

    header_coerced: dict[str, Any] = {}
    if header_field_specs:
        try:
            header_coerced = batch_coerce_scalars(
                params_to_process, header_field_specs
            )
        except Exception:
            pass

    for field in fields:
        convert_underscores = getattr(
            field.field_info, "convert_underscores", default_convert_underscores
        )
        alias = get_validation_alias(field)
        lookup_key = alias
        if convert_underscores and alias == field.name:
            lookup_key = alias.replace("_", "-")
        lookup_key_lower = lookup_key.lower()

        value = core_headers.get(lookup_key_lower)
        if value is None:
            if field.required:
                value = None  # Will be caught as missing by validator
            else:
                value = _safe_default(field.default)
                values[field.name] = value
                continue

        # Use pre-coerced value if available
        if field.name in header_coerced:
            value = header_coerced[field.name]

        field_info = field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc = (field_info.in_.value, alias)
        v_, errors_ = _validate_value_with_model_field(
            field=field, value=value, values=values, loc=loc
        )
        if errors_:
            errors.extend(errors_)
        else:
            values[field.name] = v_
    return values, errors


def _core_cookie_params_to_args(
    fields: Sequence[ModelField],
    core_cookies: dict[str, str],
) -> tuple[dict[str, Any], list[Any]]:
    """
    Extract cookie parameters using core pre-parsed cookie dict.

    The core_cookies dict maps cookie names to values (single-value per name,
    matching browser behavior where later cookies overwrite earlier ones).
    Scalar types are batch pre-coerced in core before Pydantic validation.
    """
    values: dict[str, Any] = {}
    errors: list[dict[str, Any]] = []

    if not fields:
        return values, errors

    # Batch pre-coerce scalar cookie values
    field_specs: list[tuple[str, str, str]] = []
    for field in fields:
        alias = get_validation_alias(field)
        tag = _get_scalar_type_tag(field)
        if tag and alias in core_cookies:
            field_specs.append((alias, tag, field.name))

    coerced: dict[str, Any] = {}
    if field_specs:
        try:
            coerced = batch_coerce_scalars(core_cookies, field_specs)
        except Exception:
            pass

    for field in fields:
        alias = get_validation_alias(field)
        value: Any = core_cookies.get(alias)

        if value is None:
            if field.required:
                value = None  # Will be caught as missing by validator
            else:
                value = _safe_default(field.default)
                values[field.name] = value
                continue

        # Use pre-coerced value if available
        if field.name in coerced:
            value = coerced[field.name]

        field_info = field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc = (field_info.in_.value, alias)
        v_, errors_ = _validate_value_with_model_field(
            field=field, value=value, values=values, loc=loc
        )
        if errors_:
            errors.extend(errors_)
        else:
            values[field.name] = v_
    return values, errors


def request_params_to_args(
    fields: Sequence[ModelField],
    received_params: Union[Mapping[str, Any], QueryParams, Headers],
) -> tuple[dict[str, Any], list[Any]]:
    values: dict[str, Any] = {}
    errors: list[dict[str, Any]] = []

    if not fields:
        return values, errors

    first_field = fields[0]
    fields_to_extract = fields
    single_not_embedded_field = False
    default_convert_underscores = True
    if len(fields) == 1 and lenient_issubclass(first_field.type_, BaseModel):
        fields_to_extract = get_cached_model_fields(first_field.type_)
        single_not_embedded_field = True
        # If headers are in a Pydantic model, the way to disable convert_underscores
        # would be with Header(convert_underscores=False) at the Pydantic model level
        default_convert_underscores = getattr(
            first_field.field_info, "convert_underscores", True
        )

    params_to_process: dict[str, Any] = {}

    processed_keys = set()

    for field in fields_to_extract:
        alias = None
        if isinstance(received_params, Headers):
            # Handle fields extracted from a Pydantic Model for a header, each field
            # doesn't have a FieldInfo of type Header with the default convert_underscores=True
            convert_underscores = getattr(
                field.field_info, "convert_underscores", default_convert_underscores
            )
            if convert_underscores:
                alias = get_validation_alias(field)
                if alias == field.name:
                    alias = alias.replace("_", "-")
        value = _get_multidict_value(field, received_params, alias=alias)
        if value is not None:
            params_to_process[get_validation_alias(field)] = value
        processed_keys.add(alias or get_validation_alias(field))

    for key in received_params.keys():
        if key not in processed_keys:
            if hasattr(received_params, "getlist"):
                value = received_params.getlist(key)
                if isinstance(value, list) and (len(value) == 1):
                    params_to_process[key] = value[0]
                else:
                    params_to_process[key] = value
            else:
                params_to_process[key] = received_params.get(key)

    if single_not_embedded_field:
        field_info = first_field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc: tuple[str, ...] = (field_info.in_.value,)
        v_, errors_ = _validate_value_with_model_field(
            field=first_field, value=params_to_process, values=values, loc=loc
        )
        return {first_field.name: v_}, errors_

    for field in fields:
        value = _get_multidict_value(field, received_params)
        field_info = field.field_info
        assert isinstance(field_info, params.Param), (
            "Params must be subclasses of Param"
        )
        loc = (field_info.in_.value, get_validation_alias(field))
        v_, errors_ = _validate_value_with_model_field(
            field=field, value=value, values=values, loc=loc
        )
        if errors_:
            errors.extend(errors_)
        else:
            values[field.name] = v_
    return values, errors


def is_union_of_base_models(field_type: Any) -> bool:
    """Check if field type is a Union where all members are BaseModel subclasses."""
    from fastapi.types import UnionType

    origin = get_origin(field_type)

    # Check if it's a Union type (covers both typing.Union and types.UnionType in Python 3.10+)
    if origin is not Union and origin is not UnionType:
        return False

    union_args = get_args(field_type)

    for arg in union_args:
        if not lenient_issubclass(arg, BaseModel):
            return False

    return True


def _should_embed_body_fields(fields: list[ModelField]) -> bool:
    if not fields:
        return False
    # More than one dependency could have the same field, it would show up as multiple
    # fields but it's the same one, so count them by name
    body_param_names_set = {field.name for field in fields}
    # A top level field has to be a single field, not multiple
    if len(body_param_names_set) > 1:
        return True
    first_field = fields[0]
    # If it explicitly specifies it is embedded, it has to be embedded
    if getattr(first_field.field_info, "embed", None):
        return True
    # If it's a Form (or File) field, it has to be a BaseModel (or a union of BaseModels) to be top level
    # otherwise it has to be embedded, so that the key value pair can be extracted
    if (
        isinstance(first_field.field_info, params.Form)
        and not lenient_issubclass(first_field.type_, BaseModel)
        and not is_union_of_base_models(first_field.type_)
    ):
        return True
    return False


async def _extract_form_body(
    body_fields: list[ModelField],
    received_body: FormData,
) -> dict[str, Any]:
    values = {}

    for field in body_fields:
        value = _get_multidict_value(field, received_body)
        field_info = field.field_info
        if (
            isinstance(field_info, params.File)
            and is_bytes_field(field)
            and isinstance(value, UploadFile)
        ):
            value = await value.read()
        elif (
            is_bytes_sequence_field(field)
            and isinstance(field_info, params.File)
            and value_is_sequence(value)
        ):
            # For types
            assert isinstance(value, sequence_types)
            results: list[Union[bytes, str]] = []

            async def process_fn(
                fn: Callable[[], Coroutine[Any, Any, Any]],
            ) -> None:
                result = await fn()
                results.append(result)  # noqa: B023

            async with anyio.create_task_group() as tg:
                for sub_value in value:
                    tg.start_soon(process_fn, sub_value.read)
            value = serialize_sequence_value(field=field, value=results)
        if value is not None:
            values[get_validation_alias(field)] = value
    field_aliases = {get_validation_alias(field) for field in body_fields}
    for key in received_body.keys():
        if key not in field_aliases:
            param_values = received_body.getlist(key)
            if len(param_values) == 1:
                values[key] = param_values[0]
            else:
                values[key] = param_values
    return values


async def request_body_to_args(
    body_fields: list[ModelField],
    received_body: Optional[Union[dict[str, Any], FormData]],
    embed_body_fields: bool,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    values: dict[str, Any] = {}
    errors: list[dict[str, Any]] = []
    assert body_fields, "request_body_to_args() should be called with fields"
    single_not_embedded_field = len(body_fields) == 1 and not embed_body_fields
    first_field = body_fields[0]
    body_to_process = received_body

    fields_to_extract: list[ModelField] = body_fields

    if (
        single_not_embedded_field
        and lenient_issubclass(first_field.type_, BaseModel)
        and isinstance(received_body, FormData)
    ):
        fields_to_extract = get_cached_model_fields(first_field.type_)

    if isinstance(received_body, FormData):
        body_to_process = await _extract_form_body(fields_to_extract, received_body)

    if single_not_embedded_field:
        loc: tuple[str, ...] = ("body",)
        v_, errors_ = _validate_value_with_model_field(
            field=first_field, value=body_to_process, values=values, loc=loc
        )
        return {first_field.name: v_}, errors_
    for field in body_fields:
        loc = ("body", get_validation_alias(field))
        value: Optional[Any] = None
        if body_to_process is not None:
            try:
                value = body_to_process.get(get_validation_alias(field))
            # If the received body is a list, not a dict
            except AttributeError:
                errors.append(get_missing_field_error(loc))
                continue
        v_, errors_ = _validate_value_with_model_field(
            field=field, value=value, values=values, loc=loc
        )
        if errors_:
            errors.extend(errors_)
        else:
            values[field.name] = v_
    return values, errors


def get_body_field(
    *, flat_dependant: Dependant, name: str, embed_body_fields: bool
) -> Optional[ModelField]:
    """
    Get a ModelField representing the request body for a path operation, combining
    all body parameters into a single field if necessary.

    Used to check if it's form data (with `isinstance(body_field, params.Form)`)
    or JSON and to generate the JSON Schema for a request body.

    This is **not** used to validate/parse the request body, that's done with each
    individual body parameter.
    """
    if not flat_dependant.body_params:
        return None
    first_param = flat_dependant.body_params[0]
    if not embed_body_fields:
        return first_param
    model_name = "Body_" + name
    BodyModel = create_body_model(
        fields=flat_dependant.body_params, model_name=model_name
    )
    required = any(True for f in flat_dependant.body_params if f.required)
    BodyFieldInfo_kwargs: dict[str, Any] = {
        "annotation": BodyModel,
        "alias": "body",
    }
    if not required:
        BodyFieldInfo_kwargs["default"] = None
    if any(isinstance(f.field_info, params.File) for f in flat_dependant.body_params):
        BodyFieldInfo: type[params.Body] = params.File
    elif any(isinstance(f.field_info, params.Form) for f in flat_dependant.body_params):
        BodyFieldInfo = params.Form
    else:
        BodyFieldInfo = params.Body

        body_param_media_types = [
            f.field_info.media_type
            for f in flat_dependant.body_params
            if isinstance(f.field_info, params.Body)
        ]
        if len(set(body_param_media_types)) == 1:
            BodyFieldInfo_kwargs["media_type"] = body_param_media_types[0]
    final_field = create_model_field(
        name="body",
        type_=BodyModel,
        required=required,
        alias="body",
        field_info=BodyFieldInfo(**BodyFieldInfo_kwargs),
    )
    return final_field


def get_validation_alias(field: ModelField) -> str:
    va = getattr(field, "validation_alias", None)
    return va or field.alias
