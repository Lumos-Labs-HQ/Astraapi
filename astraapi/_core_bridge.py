"""
C++ core-accelerated AstraAPI internals (v2.0).

Core extension is REQUIRED — no Python fallback.
Exported: 16 functions + 2 classes.
"""

from astraapi._astraapi_core import (  # type: ignore[import-not-found]
    CoreApp,
    InlineResult,
    ResponseData,
    openapi_dict_to_json_bytes,
    process_request,
    encode_to_json_bytes,
    parse_query_string,
    parse_scope_headers,
    parse_cookie_header,
    parse_multipart_body,
    parse_urlencoded_body,
    batch_extract_params_inline,
    batch_coerce_scalars,
    compute_dependency_order,
    fast_jsonable_encode,
    serialize_error_response,
    serialize_error_list,
    init_cached_refs,
    prewarm_buffer_pool,
)

# Run once at import time (main thread) so std::call_once in C++ completes
# before any server threads start. Prevents concurrent call_once crashes.
init_cached_refs()
