#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "router.hpp"
#include "compat.hpp"
#include <vector>
#include <string>
#include <string_view>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <optional>
#include <regex>
#include <unordered_map>
#include <unordered_set>

// ── Transparent hash/equal for string_view lookups into string sets ──────────
// Allows unordered_set<string>.find(string_view) without heap allocation.
struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
};
struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};
using TransparentStringSet = std::unordered_set<std::string, TransparentStringHash, TransparentStringEqual>;

// ── Config structs ──────────────────────────────────────────────────────────

struct CorsConfig {
    std::vector<std::string> allow_origins;
    TransparentStringSet allow_origins_set;              // O(1) origin lookup (zero-alloc with string_view)
    bool allow_any_origin = false;                       // true if "*" in origins
    std::optional<std::string> allow_origin_regex;
    std::optional<std::regex> allow_origin_regex_compiled;  // Pre-compiled at configure time
    std::vector<std::string> allow_methods;
    std::vector<std::string> allow_headers;
    bool allow_credentials = false;
    std::vector<std::string> expose_headers;
    long max_age = 600;
};

struct TrustedHostConfig {
    std::vector<std::string> allowed_hosts;
    TransparentStringSet allowed_hosts_set;              // O(1) host lookup (zero-alloc with string_view)
    bool allow_any_host = false;                         // true if "*" in hosts
};

// ── Field spec for fast-path parameter extraction ───────────────────────────

enum ParamLocation : uint8_t {
    LOC_QUERY = 0,
    LOC_HEADER = 1,
    LOC_COOKIE = 2,
    LOC_PATH = 3,
};

enum ParamType : uint8_t {
    TYPE_STR = 0,
    TYPE_INT = 1,
    TYPE_FLOAT = 2,
    TYPE_BOOL = 3,
};

struct FieldSpec {
    std::string field_name;
    std::string alias;
    std::string header_lookup_key;
    ParamLocation location;
    ParamType type_tag;
    bool required;
    PyObject* default_value;   // strong ref or NULL (INCREF'd in register_fast_spec)
    PyObject* py_field_name;   // pre-interned PyUnicode (strong ref)
};

// ── Fast-path route spec ────────────────────────────────────────────────────

struct FastRouteSpec {
    bool has_params;
    bool has_body_params;
    std::optional<std::string> body_param_name;
    bool has_header_params;
    bool has_cookie_params;
    bool has_query_params;
    // Split by location for zero-overhead iteration
    std::vector<FieldSpec> path_specs;
    std::vector<FieldSpec> query_specs;
    std::vector<FieldSpec> header_specs;
    std::vector<FieldSpec> cookie_specs;
    PyObject* body_params;  // Python list or None (strong ref)
    bool embed_body_fields;

    // O(1) lookup maps (built at registration, immutable during requests)
    // Keys are string_views into the FieldSpec vectors above (stable after registration)
    std::unordered_map<std::string_view, size_t> path_map;    // field_name → index
    std::unordered_map<std::string_view, size_t> query_map;   // alias/field_name → index
    std::unordered_map<std::string_view, size_t> header_map;  // header_lookup_key → index
    std::unordered_map<std::string_view, size_t> cookie_map;  // field_name → index

    // Dependency injection
    bool has_dependencies = false;
    PyObject* dependant;          // Python Dependant object (strong ref, or NULL)
    PyObject* dep_solver;         // Python callable: _fast_solve_deps (strong ref, or NULL)

    // Param validation (Pydantic TypeAdapters for query/header/cookie/path constraints)
    PyObject* param_validator;    // Python callable: _validate_params (strong ref, or NULL)

    // Pydantic model fast-path: call model.model_validate() directly from C++
    // Set at registration when body has exactly 1 Pydantic model param
    PyObject* model_validate;     // bound method: Model.model_validate (strong ref, or NULL)
    bool body_is_plain_dict;      // True if body param is plain dict (no Pydantic model)
    // Pre-interned body param name key — avoids PyUnicode_FromString per POST request
    PyObject* py_body_param_name; // interned PyUnicode for body_param_name (strong ref, or NULL)
};

// ── Method bitmask for O(1) method checking ─────────────────────────────────

enum MethodBit : uint8_t {
    METHOD_GET     = 1 << 0,
    METHOD_POST    = 1 << 1,
    METHOD_PUT     = 1 << 2,
    METHOD_DELETE  = 1 << 3,
    METHOD_PATCH   = 1 << 4,
    METHOD_HEAD    = 1 << 5,
    METHOD_OPTIONS = 1 << 6,
};

// ── Route info ──────────────────────────────────────────────────────────────

struct RouteInfo {
    uint64_t route_id;
    std::vector<std::string> methods;
    uint8_t method_mask = 0;        // bitmask for O(1) method check
    PyObject* endpoint;             // strong ref
    bool is_coroutine;
    uint16_t status_code;
    PyObject* response_model_field; // strong ref or NULL
    PyObject* response_class;       // strong ref or NULL
    PyObject* include;              // strong ref or NULL
    PyObject* exclude;              // strong ref or NULL
    bool exclude_unset;
    bool exclude_defaults;
    bool exclude_none;
    std::vector<std::string> tags;
    std::optional<std::string> summary;
    std::optional<std::string> description;
    std::optional<std::string> operation_id;
    bool has_body;
    bool is_form;
    std::optional<FastRouteSpec> fast_spec;
};

// ── Rate limiter shard count (outside struct — can't have static constexpr in unnamed struct)
constexpr int RATE_LIMIT_SHARDS = 16;

// ── CoreApp Python object ───────────────────────────────────────────────────

typedef struct {
    PyObject_HEAD
    Router router;
    std::vector<RouteInfo> routes;
    std::vector<std::string> route_paths;
    std::shared_mutex routes_mutex;
    std::atomic<bool> routes_frozen{false};  // Skip lock after startup
    std::atomic<std::shared_ptr<CorsConfig>> cors_config;
    bool cors_enabled = false;  // Cached bool — avoids atomic shared_ptr load per request
    const CorsConfig* cors_ptr_cached = nullptr;       // Raw pointer — set once in configure_cors
    std::atomic<std::shared_ptr<TrustedHostConfig>> trusted_host_config;
    bool trusted_host_enabled = false;                  // Cached bool — skip atomic load per request
    const TrustedHostConfig* th_ptr_cached = nullptr;   // Raw pointer — set once
    std::unordered_map<uint16_t, PyObject*> exception_handlers;
    std::atomic<uint64_t> route_counter{0};

    // ── Hot counters (no alignas — Python tp_alloc doesn't guarantee alignment) ────────
    // Was alignas(64) but that caused vmovdqa crashes since Python's allocator
    // only guarantees pointer-sized alignment, not 64-byte cache-line alignment.
    struct HotCounters {
        uint64_t total_requests = 0;
        int64_t  active_requests = 0;
        uint64_t total_errors = 0;
    };
    HotCounters counters;

    // OpenAPI schema + docs (cached as pre-built HTTP responses)
    PyObject* openapi_json_resp;    // strong ref: pre-built HTTP response bytes for /openapi.json
    PyObject* docs_html_resp;       // strong ref: pre-built HTTP response bytes for /docs
    PyObject* redoc_html_resp;      // strong ref: pre-built HTTP response bytes for /redoc
    PyObject* oauth2_redirect_html_resp;  // strong ref: pre-built HTTP response bytes for /docs/oauth2-redirect
    std::string openapi_url;        // "/openapi.json" (default)
    std::string docs_url;           // "/docs" (default)
    std::string redoc_url;          // "/redoc" (default)
    std::string oauth2_redirect_url;  // "/docs/oauth2-redirect" (default)

    // Rate limiting (C++ native) — sharded for low contention
    bool rate_limit_enabled = false;
    int rate_limit_max_requests = 100;
    int rate_limit_window_seconds = 60;
    struct RateLimitEntry { int count; int64_t window_start_ns; };
    // Transparent hash allows string_view lookups without copying the key string,
    // avoiding a per-request std::string hash+copy for the client IP.
    // (No alignas — Python tp_alloc doesn't guarantee 64-byte alignment.)
    struct RateLimitShard {
        std::mutex mutex;
        std::unordered_map<std::string, RateLimitEntry,
                           TransparentStringHash, TransparentStringEqual> counters;
    };
    RateLimitShard rate_limit_shards[RATE_LIMIT_SHARDS];
    std::string current_client_ip;
    size_t current_shard_idx = 0;  // cached shard index for current_client_ip

    // Post-response hook (for logging middleware)
    PyObject* post_response_hook = nullptr;  // Python callable or NULL

    // Connection pressure: when true, override keep_alive → false in all responses.
    // Set by Python when active_count > 80% of MAX_CONNECTIONS.
    int force_close = 0;  // int for Py_T_INT member access from Python

    // Fast-path return protocol: last sync-consumed byte count
    // Set by handle_http before returning Py_True. Python reads via tp_members.
    // Avoids per-request PyTuple_New(2) + PyLong_FromLongLong() allocation.
    Py_ssize_t last_consumed = 0;
} CoreAppObject;

// ── MatchResult Python object ───────────────────────────────────────────────

typedef struct {
    PyObject_HEAD
    Py_ssize_t route_index;
    uint64_t route_id;
    uint16_t status_code;
    bool is_coroutine;
    bool has_body;
    bool is_form;
    bool has_response_model;
    bool exclude_unset;
    bool exclude_defaults;
    bool exclude_none;
    std::vector<std::pair<std::string, std::string>> path_params;
} MatchResultObject;

// ── ResponseData Python object ──────────────────────────────────────────────

typedef struct {
    PyObject_HEAD
    uint16_t status_code;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> headers;
    std::vector<uint8_t> body;
} ResponseDataObject;

// ── InlineResult Python object — all PyObject* for T_OBJECT_EX access ─────

typedef struct {
    PyObject_HEAD
    PyObject* status_code_obj;   // PyLong (strong ref)
    PyObject* has_body_params;   // Py_True/Py_False (strong ref)
    PyObject* embed_body_fields; // Py_True/Py_False (strong ref)
    PyObject* kwargs;            // PyDict (strong ref)
    PyObject* json_body;         // strong ref
    PyObject* endpoint;          // strong ref
    PyObject* body_params;       // strong ref
} InlineResultObject;

// ── PreparedRequest Python object — returned by parse_and_route() ──────────
// All PyObject* fields for T_OBJECT_EX zero-overhead access.
// Contains everything needed to call the endpoint and serialize the response,
// without holding any lock or blocking the event loop.

typedef struct {
    PyObject_HEAD
    PyObject* kwargs;            // PyDict — extracted path/query/header/cookie params (strong ref)
    PyObject* endpoint;          // callable — the route's endpoint function (strong ref)
    PyObject* status_code_obj;   // PyLong — route's configured status code (strong ref)
    PyObject* keep_alive_obj;    // Py_True/Py_False — from parsed HTTP request (strong ref)
    PyObject* error_response;    // PyBytes — pre-built error HTTP response, or Py_None (strong ref)
    PyObject* has_body_params;   // Py_True/Py_False (strong ref)
    PyObject* body_params;       // PyList or Py_None (strong ref)
    PyObject* embed_body_fields; // Py_True/Py_False (strong ref)
    PyObject* json_body;         // parsed JSON body or Py_None (strong ref)
    PyObject* is_coroutine;      // Py_True/Py_False — whether endpoint is async (strong ref)
} PreparedRequestObject;

// ── Type objects (defined in app.cpp) ───────────────────────────────────────

extern PyTypeObject CoreAppType;
extern PyTypeObject MatchResultType;
extern PyTypeObject ResponseDataType;
extern PyTypeObject InlineResultType;
extern PyTypeObject PreparedRequestType;

// ── Module-level registration ───────────────────────────────────────────────

int register_app_types(PyObject* module);
