"""
C++ core-accelerated AstraAPI internals (v2.0).

Core extension is REQUIRED — no Python fallback.
Exported: 16 functions + 2 classes.
"""

from astraapi._astraapi_core import (  # type: ignore[import-not-found]
    # ── CoreApp ASGI core ──────────────────────────────────────────────
    CoreApp,
    InlineResult,
    ResponseData,
    # ── OpenAPI ──────────────────────────────────────────────────────────
    openapi_dict_to_json_bytes,
    # ── Request pipeline ─────────────────────────────────────────────────
    process_request,
    encode_to_json_bytes,
    # ── Parsing ──────────────────────────────────────────────────────────
    parse_query_string,
    parse_scope_headers,
    parse_cookie_header,
    parse_multipart_body,
    parse_urlencoded_body,
    # ── Parameter extraction & coercion ──────────────────────────────────
    batch_extract_params_inline,
    batch_coerce_scalars,
    # ── Dependency resolution ────────────────────────────────────────────
    compute_dependency_order,
    # ── Encoding ─────────────────────────────────────────────────────────
    fast_jsonable_encode,
    # ── Error serialization ──────────────────────────────────────────────
    serialize_error_response,
    serialize_error_list,
    # ── Warm-up / eager initialization ────────────────────────────────────
    init_cached_refs,
    prewarm_buffer_pool,
)
