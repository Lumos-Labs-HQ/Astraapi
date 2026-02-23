#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "app.hpp"
#include "asgi_constants.hpp"
#include "json_writer.hpp"
#include "json_parser.hpp"
#include "buffer_pool.hpp"
#include "http_parser.hpp"
#include "ws_frame_parser.hpp"
#include "pyref.hpp"
#include <cstring>
#include "platform.hpp"
#include <mutex>
#include <new>
#include <algorithm>
#include <string_view>
#include <chrono>
#include <zlib.h>

#if HAS_BROTLI
#include <brotli/encode.h>
#endif
#ifndef HAS_BROTLI
#define HAS_BROTLI 0
#endif

// ── Module-level cached imports (consolidated, cleaned up at exit) ────────────
static PyObject* s_http_exc_type = nullptr;         // starlette.exceptions.HTTPException
static PyObject* s_fastapi_http_exc_type = nullptr;  // fastapi.exceptions.HTTPException
static PyObject* s_resume_func = nullptr;            // fastapi._core_app._resume_coro
static PyObject* s_request_body_to_args = nullptr;   // fastapi.dependencies.utils.request_body_to_args

// Pre-interned strings for transport method calls (cleaned up at exit)
static PyObject* g_str_write = nullptr;
static PyObject* g_str_is_closing = nullptr;

// Promoted from function-local statics for eager initialization
static PyObject* s_ensure_future = nullptr;          // asyncio.ensure_future
static PyObject* s_kw_body_fields = nullptr;         // "body_fields" interned string
static PyObject* s_kw_received_body = nullptr;       // "received_body" interned string
static PyObject* s_kw_embed = nullptr;               // "embed_body_fields" interned string
static PyObject* s_detail_key_global = nullptr;      // "detail" interned string

void cleanup_cached_refs() {
    Py_CLEAR(s_http_exc_type);
    Py_CLEAR(s_fastapi_http_exc_type);
    Py_CLEAR(s_resume_func);
    Py_CLEAR(s_request_body_to_args);
    Py_CLEAR(g_str_write);
    Py_CLEAR(g_str_is_closing);
    Py_CLEAR(s_ensure_future);
    Py_CLEAR(s_kw_body_fields);
    Py_CLEAR(s_kw_received_body);
    Py_CLEAR(s_kw_embed);
    Py_CLEAR(s_detail_key_global);
}

// ── Eager initialization — called at server startup to eliminate first-request overhead ──
PyObject* py_init_cached_refs(PyObject* /*self*/, PyObject* /*args*/) {
    // Pre-intern transport method strings
    if (!g_str_write) g_str_write = PyUnicode_InternFromString("write");
    if (!g_str_is_closing) g_str_is_closing = PyUnicode_InternFromString("is_closing");

    // Pre-import exception types (avoids 3-8ms lazy import on first error)
    if (!s_http_exc_type) {
        PyRef mod(PyImport_ImportModule("starlette.exceptions"));
        if (mod) s_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    if (!s_fastapi_http_exc_type) {
        PyRef mod(PyImport_ImportModule("fastapi.exceptions"));
        if (mod) s_fastapi_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }

    // Pre-import request_body_to_args (avoids 3-8ms lazy import on first body request)
    if (!s_request_body_to_args) {
        PyRef mod(PyImport_ImportModule("fastapi.dependencies.utils"));
        if (mod) s_request_body_to_args = PyObject_GetAttrString(mod.get(), "request_body_to_args");
        else PyErr_Clear();
    }

    // Pre-import asyncio.ensure_future (avoids 2-4ms lazy import on first background task)
    if (!s_ensure_future) {
        PyRef mod(PyImport_ImportModule("asyncio"));
        if (mod) s_ensure_future = PyObject_GetAttrString(mod.get(), "ensure_future");
        else PyErr_Clear();
    }

    // Pre-intern body parsing keyword strings
    if (!s_kw_body_fields) s_kw_body_fields = PyUnicode_InternFromString("body_fields");
    if (!s_kw_received_body) s_kw_received_body = PyUnicode_InternFromString("received_body");
    if (!s_kw_embed) s_kw_embed = PyUnicode_InternFromString("embed_body_fields");
    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");

    PyErr_Clear();
    Py_RETURN_NONE;
}

// ── HTTPException type check helper ──────────────────────────────────────────
// Checks both starlette.exceptions.HTTPException and fastapi.exceptions.HTTPException
// since they are separate class hierarchies.
static bool is_http_exception(PyObject* exc_type) {
    if (!exc_type) return false;
    if (!s_http_exc_type) {
        PyRef mod(PyImport_ImportModule("starlette.exceptions"));
        if (mod) s_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    if (!s_fastapi_http_exc_type) {
        PyRef mod(PyImport_ImportModule("fastapi.exceptions"));
        if (mod) s_fastapi_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    int r;
    if (s_http_exc_type) {
        r = PyObject_IsSubclass(exc_type, s_http_exc_type);
        if (r < 0) PyErr_Clear();
        if (r == 1) return true;
    }
    if (s_fastapi_http_exc_type) {
        r = PyObject_IsSubclass(exc_type, s_fastapi_http_exc_type);
        if (r < 0) PyErr_Clear();
        if (r == 1) return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// InlineResult — ALL PyObject* fields, accessed via PyMemberDef (T_OBJECT_EX)
// Zero function call overhead for attribute access (CPython reads struct offset)
// ═══════════════════════════════════════════════════════════════════════════════

static void InlineResult_dealloc(InlineResultObject* self) {
    Py_XDECREF(self->status_code_obj);
    Py_XDECREF(self->has_body_params);
    Py_XDECREF(self->embed_body_fields);
    Py_XDECREF(self->kwargs);
    Py_XDECREF(self->json_body);
    Py_XDECREF(self->endpoint);
    Py_XDECREF(self->body_params);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

// PyMemberDef: T_OBJECT_EX = direct struct offset read, READONLY = no setter
// This replaces 7 getter functions with zero-overhead attribute access.
static PyMemberDef InlineResult_members[] = {
    {"status_code", Py_T_OBJECT_EX, offsetof(InlineResultObject, status_code_obj), Py_READONLY, nullptr},
    {"has_body_params", Py_T_OBJECT_EX, offsetof(InlineResultObject, has_body_params), Py_READONLY, nullptr},
    {"embed_body_fields", Py_T_OBJECT_EX, offsetof(InlineResultObject, embed_body_fields), Py_READONLY, nullptr},
    {"kwargs", Py_T_OBJECT_EX, offsetof(InlineResultObject, kwargs), Py_READONLY, nullptr},
    {"json_body", Py_T_OBJECT_EX, offsetof(InlineResultObject, json_body), Py_READONLY, nullptr},
    {"endpoint", Py_T_OBJECT_EX, offsetof(InlineResultObject, endpoint), Py_READONLY, nullptr},
    {"body_params", Py_T_OBJECT_EX, offsetof(InlineResultObject, body_params), Py_READONLY, nullptr},
    {nullptr}
};

PyTypeObject InlineResultType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.InlineResult",
    .tp_basicsize = sizeof(InlineResultObject),
    .tp_dealloc = (destructor)InlineResult_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_members = InlineResult_members,
};

// ═══════════════════════════════════════════════════════════════════════════════
// MatchResult
// ═══════════════════════════════════════════════════════════════════════════════

static void MatchResult_dealloc(MatchResultObject* self) {
    self->path_params.~vector();
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* MatchResult_get_route_index(MatchResultObject* self, void*) {
    return PyLong_FromSsize_t(self->route_index);
}
static PyObject* MatchResult_get_route_id(MatchResultObject* self, void*) {
    return PyLong_FromUnsignedLongLong(self->route_id);
}
static PyObject* MatchResult_get_status_code(MatchResultObject* self, void*) {
    return PyLong_FromLong(self->status_code);
}
static PyObject* MatchResult_get_is_coroutine(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->is_coroutine);
}
static PyObject* MatchResult_get_has_body(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->has_body);
}
static PyObject* MatchResult_get_is_form(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->is_form);
}
static PyObject* MatchResult_get_has_response_model(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->has_response_model);
}
static PyObject* MatchResult_get_exclude_unset(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->exclude_unset);
}
static PyObject* MatchResult_get_exclude_defaults(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->exclude_defaults);
}
static PyObject* MatchResult_get_exclude_none(MatchResultObject* self, void*) {
    return PyBool_FromLong(self->exclude_none);
}

static PyObject* MatchResult_get_path_params(MatchResultObject* self, PyObject*) {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;
    for (const auto& [k, v] : self->path_params) {
        PyRef key(PyUnicode_FromStringAndSize(k.c_str(), k.size()));
        PyRef val(PyUnicode_FromStringAndSize(v.c_str(), v.size()));
        if (!key || !val || PyDict_SetItem(dict, key.get(), val.get()) < 0) {
            Py_DECREF(dict);
            return nullptr;
        }
    }
    return dict;
}

static PyGetSetDef MatchResult_getset[] = {
    {"route_index", (getter)MatchResult_get_route_index, nullptr, nullptr, nullptr},
    {"route_id", (getter)MatchResult_get_route_id, nullptr, nullptr, nullptr},
    {"status_code", (getter)MatchResult_get_status_code, nullptr, nullptr, nullptr},
    {"is_coroutine", (getter)MatchResult_get_is_coroutine, nullptr, nullptr, nullptr},
    {"has_body", (getter)MatchResult_get_has_body, nullptr, nullptr, nullptr},
    {"is_form", (getter)MatchResult_get_is_form, nullptr, nullptr, nullptr},
    {"has_response_model", (getter)MatchResult_get_has_response_model, nullptr, nullptr, nullptr},
    {"exclude_unset", (getter)MatchResult_get_exclude_unset, nullptr, nullptr, nullptr},
    {"exclude_defaults", (getter)MatchResult_get_exclude_defaults, nullptr, nullptr, nullptr},
    {"exclude_none", (getter)MatchResult_get_exclude_none, nullptr, nullptr, nullptr},
    {nullptr}
};

static PyMethodDef MatchResult_methods[] = {
    {"get_path_params", (PyCFunction)MatchResult_get_path_params, METH_NOARGS, nullptr},
    {nullptr}
};

PyTypeObject MatchResultType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.MatchResult",
    .tp_basicsize = sizeof(MatchResultObject),
    .tp_dealloc = (destructor)MatchResult_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = MatchResult_methods,
    .tp_getset = MatchResult_getset,
};

// ═══════════════════════════════════════════════════════════════════════════════
// ResponseData
// ═══════════════════════════════════════════════════════════════════════════════

static void ResponseData_dealloc(ResponseDataObject* self) {
    self->headers.~vector();
    self->body.~vector();
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* ResponseData_get_status(ResponseDataObject* self, void*) {
    return PyLong_FromLong(self->status_code);
}

static PyObject* ResponseData_start_message(ResponseDataObject* self, PyObject*) {
    PyRef msg(PyDict_New());
    if (!msg) return nullptr;

    if (PyDict_SetItem(msg.get(), g_str_type, g_str_http_response_start) < 0) return nullptr;
    PyRef status_obj(PyLong_FromLong(self->status_code));
    if (PyDict_SetItem(msg.get(), g_str_status, status_obj.get()) < 0) return nullptr;

    PyRef headers_list(PyList_New((Py_ssize_t)self->headers.size()));
    if (!headers_list) return nullptr;

    for (size_t i = 0; i < self->headers.size(); i++) {
        const auto& [name, value] = self->headers[i];
        PyRef pair(PyList_New(2));
        if (!pair) return nullptr;
        PyObject* name_bytes = PyBytes_FromStringAndSize(
            (const char*)name.data(), (Py_ssize_t)name.size());
        PyObject* val_bytes = PyBytes_FromStringAndSize(
            (const char*)value.data(), (Py_ssize_t)value.size());
        if (!name_bytes || !val_bytes) { Py_XDECREF(name_bytes); Py_XDECREF(val_bytes); return nullptr; }
        PyList_SET_ITEM(pair.get(), 0, name_bytes);
        PyList_SET_ITEM(pair.get(), 1, val_bytes);
        PyList_SET_ITEM(headers_list.get(), (Py_ssize_t)i, pair.release());
    }

    if (PyDict_SetItem(msg.get(), g_str_headers, headers_list.get()) < 0) return nullptr;

    return msg.release();
}

static PyObject* ResponseData_body_message(ResponseDataObject* self, PyObject*) {
    PyRef msg(PyDict_New());
    if (!msg) return nullptr;

    if (PyDict_SetItem(msg.get(), g_str_type, g_str_http_response_body) < 0) return nullptr;
    PyRef body_bytes(PyBytes_FromStringAndSize(
        (const char*)self->body.data(), (Py_ssize_t)self->body.size()));
    if (!body_bytes) return nullptr;
    if (PyDict_SetItem(msg.get(), g_str_body, body_bytes.get()) < 0) return nullptr;

    return msg.release();
}

static PyObject* ResponseData_get_body(ResponseDataObject* self, PyObject*) {
    return PyBytes_FromStringAndSize(
        (const char*)self->body.data(), (Py_ssize_t)self->body.size());
}

static PyGetSetDef ResponseData_getset[] = {
    {"status", (getter)ResponseData_get_status, nullptr, nullptr, nullptr},
    {nullptr}
};

static PyMethodDef ResponseData_methods[] = {
    {"start_message", (PyCFunction)ResponseData_start_message, METH_NOARGS, nullptr},
    {"body_message", (PyCFunction)ResponseData_body_message, METH_NOARGS, nullptr},
    {"get_body", (PyCFunction)ResponseData_get_body, METH_NOARGS, nullptr},
    {nullptr}
};

PyTypeObject ResponseDataType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.ResponseData",
    .tp_basicsize = sizeof(ResponseDataObject),
    .tp_dealloc = (destructor)ResponseData_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = ResponseData_methods,
    .tp_getset = ResponseData_getset,
};

// ═══════════════════════════════════════════════════════════════════════════════
// CoreApp
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_new(PyTypeObject* type, PyObject*, PyObject*) {
    CoreAppObject* self = (CoreAppObject*)type->tp_alloc(type, 0);
    if (self) {
        new (&self->router) Router();
        new (&self->routes) std::vector<RouteInfo>();
        new (&self->route_paths) std::vector<std::string>();
        new (&self->routes_mutex) std::shared_mutex();
        new (&self->cors_config) std::atomic<std::shared_ptr<CorsConfig>>();
        new (&self->trusted_host_config) std::atomic<std::shared_ptr<TrustedHostConfig>>();
        new (&self->exception_handlers) std::unordered_map<uint16_t, PyObject*>();
        self->route_counter.store(0);
        self->total_requests.store(0);
        self->active_requests.store(0);
        self->total_errors.store(0);
        self->openapi_json_resp = nullptr;
        self->docs_html_resp = nullptr;
        self->redoc_html_resp = nullptr;
        new (&self->openapi_url) std::string("/openapi.json");
        new (&self->docs_url) std::string("/docs");
        new (&self->redoc_url) std::string("/redoc");
        // Rate limiting + logging hook
        self->rate_limit_enabled = false;
        self->rate_limit_max_requests = 100;
        self->rate_limit_window_seconds = 60;
        for (int i = 0; i < RATE_LIMIT_SHARDS; i++) {
            new (&self->rate_limit_shards[i].mutex) std::mutex();
            new (&self->rate_limit_shards[i].counters) std::unordered_map<std::string, CoreAppObject::RateLimitEntry>();
        }
        new (&self->current_client_ip) std::string();
        self->post_response_hook = nullptr;
    }
    return (PyObject*)self;
}

static void CoreApp_dealloc(CoreAppObject* self) {
    // Release Python refs in routes
    for (auto& route : self->routes) {
        Py_XDECREF(route.endpoint);
        Py_XDECREF(route.response_model_field);
        Py_XDECREF(route.response_class);
        Py_XDECREF(route.include);
        Py_XDECREF(route.exclude);
        if (route.fast_spec) {
            Py_XDECREF(route.fast_spec->body_params);
            Py_XDECREF(route.fast_spec->dependant);
            Py_XDECREF(route.fast_spec->dep_solver);
            Py_XDECREF(route.fast_spec->model_validate);
            // Release pre-interned py_field_name + default_value refs
            auto release_specs = [](std::vector<FieldSpec>& specs) {
                for (auto& fs : specs) {
                    Py_XDECREF(fs.py_field_name);
                    Py_XDECREF(fs.default_value);
                }
            };
            release_specs(route.fast_spec->path_specs);
            release_specs(route.fast_spec->query_specs);
            release_specs(route.fast_spec->header_specs);
            release_specs(route.fast_spec->cookie_specs);
        }
    }
    for (auto& [_, handler] : self->exception_handlers) {
        Py_XDECREF(handler);
    }
    self->router.~Router();
    self->routes.~vector();
    self->route_paths.~vector();
    self->routes_mutex.~shared_mutex();
    self->cors_config.~atomic();
    self->trusted_host_config.~atomic();
    self->exception_handlers.~unordered_map();
    Py_XDECREF(self->openapi_json_resp);
    Py_XDECREF(self->docs_html_resp);
    Py_XDECREF(self->redoc_html_resp);
    self->openapi_url.~basic_string();
    self->docs_url.~basic_string();
    self->redoc_url.~basic_string();
    Py_XDECREF(self->post_response_hook);
    for (int i = 0; i < RATE_LIMIT_SHARDS; i++) {
        self->rate_limit_shards[i].counters.~unordered_map();
        self->rate_limit_shards[i].mutex.~mutex();
    }
    self->current_client_ip.~basic_string();
    Py_TYPE(self)->tp_free((PyObject*)self);
}

// ── Forward declarations ─────────────────────────────────────────────────────
static const char* status_reason(int code);

// Forward declarations for pre-cached status lines (defined after status_reason)
struct CachedStatusLine { const char* data; size_t len; };
struct CachedJsonPrefix { const char* data; size_t len; };
static CachedStatusLine s_status_lines[600];
static CachedJsonPrefix s_json_prefixes[600];

// ── CoreApp methods ─────────────────────────────────────────────────────────

// Build a complete HTTP response for text/html or application/json content
static PyObject* build_static_response(
    int status_code, const char* content_type, const char* body, size_t body_len)
{
    auto buf = acquire_buffer();
    buf.reserve(256 + body_len);

    // Use pre-cached status line if available
    if (status_code >= 0 && status_code < 600 && s_status_lines[status_code].data) {
        const auto& sl = s_status_lines[status_code];
        buf.insert(buf.end(), sl.data, sl.data + sl.len - 2);  // exclude \r\n (added with ct_pre below)
    } else {
        static const char prefix[] = "HTTP/1.1 ";
        buf.insert(buf.end(), prefix, prefix + sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf.insert(buf.end(), sc_buf, sc_buf + sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf.insert(buf.end(), reason, reason + rlen);
    }

    // Content-Type header
    const char* ct_pre = "\r\ncontent-type: ";
    buf.insert(buf.end(), ct_pre, ct_pre + 16);
    size_t ct_len = strlen(content_type);
    buf.insert(buf.end(), content_type, content_type + ct_len);

    // Content-Length header
    const char* cl_pre = "\r\ncontent-length: ";
    buf.insert(buf.end(), cl_pre, cl_pre + 18);
    char cl_buf[20];
    int cl_n = fast_i64_to_buf(cl_buf, (long long)body_len);
    buf.insert(buf.end(), cl_buf, cl_buf + cl_n);

    // Connection + end of headers
    static const char end_hdr[] = "\r\nconnection: keep-alive\r\n\r\n";
    buf.insert(buf.end(), end_hdr, end_hdr + sizeof(end_hdr) - 1);

    // Body
    buf.insert(buf.end(), body, body + body_len);

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// Swagger UI HTML template
static const char SWAGGER_UI_HTML[] = R"(<!DOCTYPE html>
<html>
<head>
<title>FastAPI - Swagger UI</title>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" type="text/css" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
<div id="swagger-ui"></div>
<script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
<script>
SwaggerUIBundle({url: "/openapi.json", dom_id: "#swagger-ui", presets: [SwaggerUIBundle.presets.apis, SwaggerUIBundle.SwaggerUIStandalonePreset], layout: "BaseLayout"})
</script>
</body>
</html>)";

// ReDoc HTML template
static const char REDOC_HTML[] = R"(<!DOCTYPE html>
<html>
<head>
<title>FastAPI - ReDoc</title>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link href="https://fonts.googleapis.com/css?family=Montserrat:300,400,700|Roboto:300,400,700" rel="stylesheet">
</head>
<body>
<redoc spec-url="/openapi.json"></redoc>
<script src="https://cdn.redoc.ly/redoc/latest/bundles/redoc.standalone.js"></script>
</body>
</html>)";

static PyObject* CoreApp_set_openapi_schema(CoreAppObject* self, PyObject* arg) {
    // arg = OpenAPI schema as JSON string (Python str)
    if (!PyUnicode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected str (JSON schema)");
        return nullptr;
    }

    Py_ssize_t json_len;
    const char* json_data = PyUnicode_AsUTF8AndSize(arg, &json_len);
    if (!json_data) return nullptr;

    // Build cached /openapi.json response
    Py_XDECREF(self->openapi_json_resp);
    self->openapi_json_resp = build_static_response(
        200, "application/json", json_data, (size_t)json_len);

    // Build cached /docs response (Swagger UI)
    Py_XDECREF(self->docs_html_resp);
    self->docs_html_resp = build_static_response(
        200, "text/html; charset=utf-8",
        SWAGGER_UI_HTML, sizeof(SWAGGER_UI_HTML) - 1);

    // Build cached /redoc response
    Py_XDECREF(self->redoc_html_resp);
    self->redoc_html_resp = build_static_response(
        200, "text/html; charset=utf-8",
        REDOC_HTML, sizeof(REDOC_HTML) - 1);

    Py_RETURN_NONE;
}

static PyObject* CoreApp_next_route_id(CoreAppObject* self, PyObject*) {
    uint64_t id = self->route_counter.fetch_add(1, std::memory_order_relaxed);
    return PyLong_FromUnsignedLongLong(id);
}

static PyObject* CoreApp_record_request_start(CoreAppObject* self, PyObject*) {
    self->total_requests.fetch_add(1, std::memory_order_relaxed);
    self->active_requests.fetch_add(1, std::memory_order_relaxed);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_record_request_end(CoreAppObject* self, PyObject*) {
    self->active_requests.fetch_sub(1, std::memory_order_release);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_record_error(CoreAppObject* self, PyObject*) {
    self->total_errors.fetch_add(1, std::memory_order_relaxed);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_route_count(CoreAppObject* self, PyObject*) {
    std::shared_lock lock(self->routes_mutex);
    return PyLong_FromSsize_t((Py_ssize_t)self->routes.size());
}

static PyObject* CoreApp_freeze_routes(CoreAppObject* self, PyObject*) {
    self->routes_frozen.store(true, std::memory_order_release);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_get_metrics(CoreAppObject* self, PyObject*) {
    PyRef dict(PyDict_New());
    if (!dict) return nullptr;
    PyRef tr(PyLong_FromUnsignedLongLong(self->total_requests.load(std::memory_order_relaxed)));
    PyRef ar(PyLong_FromLongLong(self->active_requests.load(std::memory_order_acquire)));
    PyRef te(PyLong_FromUnsignedLongLong(self->total_errors.load(std::memory_order_relaxed)));
    std::shared_lock lock(self->routes_mutex);
    PyRef rc(PyLong_FromSsize_t((Py_ssize_t)self->routes.size()));
    lock.unlock();
    if (!tr || !ar || !te || !rc) return nullptr;
    if (PyDict_SetItemString(dict.get(), "total_requests", tr.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "active_requests", ar.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "total_errors", te.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "route_count", rc.get()) < 0) return nullptr;
    return dict.release();
}

static PyObject* CoreApp_get_routes(CoreAppObject* self, PyObject*) {
    std::shared_lock lock(self->routes_mutex);
    PyRef list(PyList_New((Py_ssize_t)self->route_paths.size()));
    if (!list) return nullptr;
    for (size_t i = 0; i < self->route_paths.size(); i++) {
        PyObject* s = PyUnicode_FromStringAndSize(
            self->route_paths[i].c_str(), (Py_ssize_t)self->route_paths[i].size());
        if (!s) return nullptr;
        PyList_SET_ITEM(list.get(), (Py_ssize_t)i, s);
    }
    return list.release();
}

// ── Method string → bitmask helpers ─────────────────────────────────────────
// Convert HTTP method string to MethodBit bitmask. Returns 0 for unrecognized.
// Expects uppercase input (as sent over HTTP per RFC 7230 §3.1.1).

static inline uint8_t method_str_to_bit(const char* method, size_t len) {
    switch (len) {
        case 3:
            if (memcmp(method, "GET", 3) == 0) return METHOD_GET;
            if (memcmp(method, "PUT", 3) == 0) return METHOD_PUT;
            break;
        case 4:
            if (memcmp(method, "POST", 4) == 0) return METHOD_POST;
            if (memcmp(method, "HEAD", 4) == 0) return METHOD_HEAD;
            break;
        case 5:
            if (memcmp(method, "PATCH", 5) == 0) return METHOD_PATCH;
            break;
        case 6:
            if (memcmp(method, "DELETE", 6) == 0) return METHOD_DELETE;
            break;
        case 7:
            if (memcmp(method, "OPTIONS", 7) == 0) return METHOD_OPTIONS;
            break;
    }
    return 0;
}

// Case-insensitive variant for route registration (methods may arrive lowercase).
static inline uint8_t method_str_to_bit_ci(const char* method, size_t len) {
    if (len == 0 || len > 7) return 0;
    // Uppercase into stack buffer
    char upper[8];
    for (size_t i = 0; i < len; i++) {
        upper[i] = (method[i] >= 'a' && method[i] <= 'z')
            ? static_cast<char>(method[i] - 32) : method[i];
    }
    return method_str_to_bit(upper, len);
}

// ── add_route ───────────────────────────────────────────────────────────────

static PyObject* CoreApp_add_route(CoreAppObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "path", "methods", "endpoint", "is_coroutine", "status_code",
        "response_model_field", "response_class", "include", "exclude",
        "exclude_unset", "exclude_defaults", "exclude_none",
        "tags", "summary", "description", "operation_id",
        "has_body", "is_form", nullptr
    };

    const char* path = nullptr;
    PyObject* methods_list = nullptr;
    PyObject* endpoint = nullptr;
    int is_coroutine = 0;
    int status_code = 200;
    PyObject* response_model_field = Py_None;
    PyObject* response_class = Py_None;
    PyObject* include = Py_None;
    PyObject* exclude = Py_None;
    int exclude_unset = 0, exclude_defaults = 0, exclude_none = 0;
    PyObject* tags_list = Py_None;
    const char* summary = nullptr;
    const char* description = nullptr;
    const char* operation_id = nullptr;
    int has_body = 0, is_form = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "sOOp|HOOOOpppOzzzpp", (char**)kwlist,
            &path, &methods_list, &endpoint, &is_coroutine,
            &status_code, &response_model_field, &response_class,
            &include, &exclude, &exclude_unset, &exclude_defaults, &exclude_none,
            &tags_list, &summary, &description, &operation_id,
            &has_body, &is_form)) {
        return nullptr;
    }

    RouteInfo route;
    route.route_id = self->route_counter.fetch_add(1, std::memory_order_relaxed);

    Py_ssize_t nmethod = PyList_Size(methods_list);
    for (Py_ssize_t i = 0; i < nmethod; i++) {
        PyObject* m = PyList_GET_ITEM(methods_list, i);
        const char* ms = PyUnicode_AsUTF8(m);
        if (!ms) return nullptr;
        route.methods.emplace_back(ms);
        // Build method bitmask for O(1) method checking (case-insensitive for registration)
        route.method_mask |= method_str_to_bit_ci(ms, strlen(ms));
    }

    Py_INCREF(endpoint);
    route.endpoint = endpoint;
    route.is_coroutine = (bool)is_coroutine;
    route.status_code = (uint16_t)status_code;

    route.response_model_field = (response_model_field != Py_None) ? (Py_INCREF(response_model_field), response_model_field) : nullptr;
    route.response_class = (response_class != Py_None) ? (Py_INCREF(response_class), response_class) : nullptr;
    route.include = (include != Py_None) ? (Py_INCREF(include), include) : nullptr;
    route.exclude = (exclude != Py_None) ? (Py_INCREF(exclude), exclude) : nullptr;
    route.exclude_unset = (bool)exclude_unset;
    route.exclude_defaults = (bool)exclude_defaults;
    route.exclude_none = (bool)exclude_none;

    if (tags_list != Py_None && PyList_Check(tags_list)) {
        Py_ssize_t ntags = PyList_Size(tags_list);
        for (Py_ssize_t i = 0; i < ntags; i++) {
            const char* t = PyUnicode_AsUTF8(PyList_GET_ITEM(tags_list, i));
            if (t) route.tags.emplace_back(t);
        }
    }

    route.summary = summary ? std::optional<std::string>(summary) : std::nullopt;
    route.description = description ? std::optional<std::string>(description) : std::nullopt;
    route.operation_id = operation_id ? std::optional<std::string>(operation_id) : std::nullopt;
    route.has_body = (bool)has_body;
    route.is_form = (bool)is_form;

    {
        std::unique_lock lock(self->routes_mutex);
        int idx = (int)self->routes.size();
        self->router.insert(path, idx);
        self->routes.push_back(std::move(route));
        self->route_paths.emplace_back(path);
    }

    return PyLong_FromUnsignedLongLong(route.route_id);
}

// ── match_request ───────────────────────────────────────────────────────────

static PyObject* CoreApp_match_request(CoreAppObject* self, PyObject* args) {
    const char* method;
    const char* path;
    if (!PyArg_ParseTuple(args, "ss", &method, &path)) return nullptr;

    std::shared_lock lock(self->routes_mutex);
    auto match = self->router.at(path, strlen(path));
    if (!match) {
        lock.unlock();
        Py_RETURN_NONE;
    }

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) {
        lock.unlock();
        Py_RETURN_NONE;
    }

    const auto& route = self->routes[idx];

    if (route.method_mask) {
        uint8_t req_method = method_str_to_bit_ci(method, strlen(method));
        if (!(route.method_mask & req_method)) { lock.unlock(); Py_RETURN_NONE; }
    }

    MatchResultObject* mr = PyObject_New(MatchResultObject, &MatchResultType);
    if (!mr) return nullptr;
    mr->route_index = (Py_ssize_t)idx;
    mr->route_id = route.route_id;
    mr->status_code = route.status_code;
    mr->is_coroutine = route.is_coroutine;
    mr->has_body = route.has_body;
    mr->is_form = route.is_form;
    mr->has_response_model = (route.response_model_field != nullptr);
    mr->exclude_unset = route.exclude_unset;
    mr->exclude_defaults = route.exclude_defaults;
    mr->exclude_none = route.exclude_none;
    new (&mr->path_params) std::vector<std::pair<std::string, std::string>>();
    mr->path_params.reserve(match->param_count);
    for (int i = 0; i < match->param_count; i++) {
        mr->path_params.emplace_back(
            std::string(match->params[i].name),
            std::string(match->params[i].value));
    }

    return (PyObject*)mr;
}

// ── get_endpoint ────────────────────────────────────────────────────────────

static PyObject* CoreApp_get_endpoint(CoreAppObject* self, PyObject* arg) {
    Py_ssize_t idx = PyLong_AsSsize_t(arg);
    if (idx < 0 && PyErr_Occurred()) return nullptr;

    std::shared_lock lock(self->routes_mutex);
    if (idx < 0 || idx >= (Py_ssize_t)self->routes.size()) {
        PyErr_SetString(PyExc_IndexError, "route index out of range");
        return nullptr;
    }
    PyObject* ep = self->routes[idx].endpoint;
    Py_INCREF(ep);
    return ep;
}

// ── get_response_model_field ────────────────────────────────────────────────

static PyObject* CoreApp_get_response_model_field(CoreAppObject* self, PyObject* arg) {
    Py_ssize_t idx = PyLong_AsSsize_t(arg);
    if (idx < 0 && PyErr_Occurred()) return nullptr;

    std::shared_lock lock(self->routes_mutex);
    if (idx < 0 || idx >= (Py_ssize_t)self->routes.size()) Py_RETURN_NONE;
    PyObject* f = self->routes[idx].response_model_field;
    if (!f) Py_RETURN_NONE;
    Py_INCREF(f);
    return f;
}

// ── get_response_filters ────────────────────────────────────────────────────

static PyObject* CoreApp_get_response_filters(CoreAppObject* self, PyObject* arg) {
    Py_ssize_t idx = PyLong_AsSsize_t(arg);
    if (idx < 0 && PyErr_Occurred()) return nullptr;

    std::shared_lock lock(self->routes_mutex);
    if (idx < 0 || idx >= (Py_ssize_t)self->routes.size()) {
        return Py_BuildValue("(OO)", Py_None, Py_None);
    }
    const auto& route = self->routes[idx];
    PyObject* inc = route.include ? route.include : Py_None;
    PyObject* exc = route.exclude ? route.exclude : Py_None;
    return PyTuple_Pack(2, inc, exc);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: coerce string_view value to PyObject* based on ParamType
// ═══════════════════════════════════════════════════════════════════════════════

static inline PyObject* coerce_param(std::string_view val, ParamType type_tag) {
    switch (type_tag) {
        case TYPE_INT: {
            // Ultra-fast path: small positive integers (1-6 digits, no sign)
            // Covers 99%+ of path/query integer params (IDs, page, limit, etc.)
            if (val.size() >= 1 && val.size() <= 6) {
                long v = 0;
                bool all_digit = true;
                for (char c : val) {
                    if (c < '0' || c > '9') { all_digit = false; break; }
                    v = v * 10 + (c - '0');
                }
                if (all_digit) return PyLong_FromLong(v);
            }
            // Fast path: fits in stack buffer (handles negatives, larger numbers)
            if (val.size() <= 20) {
                char buf[24];
                memcpy(buf, val.data(), val.size());
                buf[val.size()] = '\0';
                char* endptr = nullptr;
                long long v = strtoll(buf, &endptr, 10);
                if (endptr == buf + val.size()) {
                    return PyLong_FromLongLong(v);
                }
            }
            // Fallback: use Python int() for arbitrary-length or non-numeric values
            PyRef py_str(PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size()));
            if (!py_str) return nullptr;
            return PyLong_FromUnicodeObject(py_str.get(), 10);
        }
        case TYPE_FLOAT: {
            // Fast path: fits in stack buffer
            if (val.size() <= 48) {
                char buf[52];
                memcpy(buf, val.data(), val.size());
                buf[val.size()] = '\0';
                char* endptr = nullptr;
                double v = strtod(buf, &endptr);
                if (endptr == buf + val.size()) {
                    return PyFloat_FromDouble(v);
                }
            }
            // Fallback: use Python float() for arbitrary-length values
            PyRef py_str(PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size()));
            if (!py_str) return nullptr;
            return PyFloat_FromString(py_str.get());
        }
        case TYPE_BOOL: {
            PyObject* r = (val == "true" || val == "1" || val == "True")
                ? Py_True : Py_False;
            Py_INCREF(r);
            return r;
        }
        default:
            return PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size());
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: Log Python error to stderr before clearing (replaces silent PyErr_Clear)
// ═══════════════════════════════════════════════════════════════════════════════

static void log_and_clear_pyerr(const char* context) {
    if (!PyErr_Occurred()) return;
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    PyRef val_str(value ? PyObject_Str(value) : nullptr);
    const char* msg = val_str ? PyUnicode_AsUTF8(val_str.get()) : "<unknown>";
    fprintf(stderr, "[fastapi-cpp] %s: %s\n", context, msg ? msg : "<unknown>");
    Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: Build 500 error tuple from pre-built constants
// ═══════════════════════════════════════════════════════════════════════════════

static inline PyObject* build_500_tuple() {
    // PyTuple_Pack INCREFs each item internally — no explicit INCREF needed
    return PyTuple_Pack(2, g_500_start, g_500_body);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: Fast (consumed, True) tuple — avoids PyTuple_Pack varargs overhead
// ═══════════════════════════════════════════════════════════════════════════════

static inline PyObject* make_consumed_true(size_t consumed) {
    PyObject* c = PyLong_FromLongLong((long long)consumed);
    if (!c) return nullptr;
    PyObject* t = PyTuple_New(2);
    if (!t) { Py_DECREF(c); return nullptr; }
    PyTuple_SET_ITEM(t, 0, c);
    Py_INCREF(Py_True);
    PyTuple_SET_ITEM(t, 1, Py_True);
    return t;
}

// HELPER: Fast (consumed, obj) tuple — obj ref is stolen
static inline PyObject* make_consumed_obj(size_t consumed, PyObject* obj) {
    PyObject* c = PyLong_FromLongLong((long long)consumed);
    if (!c) { Py_DECREF(obj); return nullptr; }
    PyObject* t = PyTuple_New(2);
    if (!t) { Py_DECREF(c); Py_DECREF(obj); return nullptr; }
    PyTuple_SET_ITEM(t, 0, c);
    PyTuple_SET_ITEM(t, 1, obj);
    return t;
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: Build error response tuple for HTTPException
// ═══════════════════════════════════════════════════════════════════════════════

static inline PyObject* build_error_response(PyObject* detail, int status_code) {
    PyRef content(PyDict_New());
    if (!content) return build_500_tuple();

    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");

    if (PyDict_SetItem(content.get(), s_detail_key_global, detail) < 0) return build_500_tuple();

    PyRef body_bytes(serialize_to_json_pybytes(content.get()));
    if (!body_bytes) return build_500_tuple();

    Py_ssize_t body_len = PyBytes_GET_SIZE(body_bytes.get());

    // Start dict (direct construction)
    PyRef start(PyDict_New());
    if (!start) return build_500_tuple();
    if (PyDict_SetItem(start.get(), g_str_type, g_str_http_response_start) < 0) return build_500_tuple();

    PyRef status_obj(get_cached_status(status_code));
    if (PyDict_SetItem(start.get(), g_str_status, status_obj.get()) < 0) return build_500_tuple();

    // Headers
    char len_buf[21];
    int len_n = fast_i64_to_buf(len_buf, body_len);
    PyRef headers_list(PyList_New(2));
    if (!headers_list) return build_500_tuple();
    Py_INCREF(g_ct_json_header_pair);
    PyList_SET_ITEM(headers_list.get(), 0, g_ct_json_header_pair);
    PyRef cl_pair(PyList_New(2));
    if (!cl_pair) return build_500_tuple();
    Py_INCREF(g_bytes_content_length);
    PyList_SET_ITEM(cl_pair.get(), 0, g_bytes_content_length);
    PyObject* cl_val = PyBytes_FromStringAndSize(len_buf, len_n);
    if (!cl_val) return build_500_tuple();
    PyList_SET_ITEM(cl_pair.get(), 1, cl_val);
    PyList_SET_ITEM(headers_list.get(), 1, cl_pair.release());
    if (PyDict_SetItem(start.get(), g_str_headers, headers_list.get()) < 0) return build_500_tuple();

    // Body dict (direct construction)
    PyRef body_msg(PyDict_New());
    if (!body_msg) return build_500_tuple();
    if (PyDict_SetItem(body_msg.get(), g_str_type, g_str_http_response_body) < 0) return build_500_tuple();
    if (PyDict_SetItem(body_msg.get(), g_str_body, body_bytes.get()) < 0) return build_500_tuple();

    return PyTuple_Pack(2, start.get(), body_msg.get());
}

// ── register_fast_spec ──────────────────────────────────────────────────────

static PyObject* CoreApp_register_fast_spec(CoreAppObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "route_index", "body_param_name", "field_specs_list",
        "body_params", "embed_body_fields", "dependant", "dep_solver", nullptr
    };

    Py_ssize_t route_index;
    const char* body_param_name = nullptr;
    PyObject* field_specs_list = Py_None;
    PyObject* body_params_obj = Py_None;
    int embed_body_fields = 0;
    PyObject* dependant_obj = Py_None;
    PyObject* dep_solver_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "n|zOOpOO", (char**)kwlist,
            &route_index, &body_param_name, &field_specs_list,
            &body_params_obj, &embed_body_fields,
            &dependant_obj, &dep_solver_obj)) {
        return nullptr;
    }

    std::unique_lock lock(self->routes_mutex);
    if (route_index < 0 || route_index >= (Py_ssize_t)self->routes.size()) {
        PyErr_SetString(PyExc_IndexError, "route index out of range");
        return nullptr;
    }

    auto& route = self->routes[route_index];
    FastRouteSpec spec;

    spec.body_param_name = body_param_name ? std::optional<std::string>(body_param_name) : std::nullopt;
    spec.embed_body_fields = (bool)embed_body_fields;
    spec.has_body_params = false;
    spec.has_header_params = false;
    spec.has_cookie_params = false;
    spec.has_query_params = false;
    spec.has_params = false;

    // Parse field specs and split into location-specific vectors
    if (field_specs_list != Py_None && PyList_Check(field_specs_list)) {
        Py_ssize_t nspecs = PyList_GET_SIZE(field_specs_list);
        for (Py_ssize_t i = 0; i < nspecs; i++) {
            PyObject* spec_dict = PyList_GET_ITEM(field_specs_list, i);
            if (!PyDict_Check(spec_dict)) continue;

            FieldSpec fs;
            PyObject* fn = PyDict_GetItemString(spec_dict, "field_name");
            PyObject* al = PyDict_GetItemString(spec_dict, "alias");
            PyObject* hlk = PyDict_GetItemString(spec_dict, "header_lookup_key");
            PyObject* loc = PyDict_GetItemString(spec_dict, "location");
            PyObject* tt = PyDict_GetItemString(spec_dict, "type_tag");
            PyObject* req = PyDict_GetItemString(spec_dict, "required");
            PyObject* def = PyDict_GetItemString(spec_dict, "default_value");

            fs.field_name = fn ? PyUnicode_AsUTF8(fn) : "";
            fs.alias = al ? PyUnicode_AsUTF8(al) : fs.field_name;
            fs.header_lookup_key = hlk ? PyUnicode_AsUTF8(hlk) : "";
            fs.location = loc ? (ParamLocation)PyLong_AsLong(loc) : LOC_QUERY;
            fs.type_tag = tt ? (ParamType)PyLong_AsLong(tt) : TYPE_STR;
            fs.required = req ? PyObject_IsTrue(req) : false;
            if (def && def != Py_None) {
                Py_INCREF(def);
                fs.default_value = def;  // strong ref — DECREF'd in CoreApp_dealloc
            } else {
                fs.default_value = nullptr;
            }

            // Pre-intern the field name for zero-alloc dict key insertion
            fs.py_field_name = PyUnicode_InternFromString(fs.field_name.c_str());

            spec.has_params = true;

            // Split into location-specific vector
            switch (fs.location) {
                case LOC_PATH:
                    spec.path_specs.push_back(std::move(fs));
                    break;
                case LOC_QUERY:
                    spec.has_query_params = true;
                    spec.query_specs.push_back(std::move(fs));
                    break;
                case LOC_HEADER:
                    spec.has_header_params = true;
                    spec.header_specs.push_back(std::move(fs));
                    break;
                case LOC_COOKIE:
                    spec.has_cookie_params = true;
                    spec.cookie_specs.push_back(std::move(fs));
                    break;
            }
        }
    }

    // Pydantic model fast-path init
    spec.model_validate = nullptr;
    spec.body_is_plain_dict = false;

    if (body_params_obj != Py_None) {
        Py_INCREF(body_params_obj);
        spec.body_params = body_params_obj;
        spec.has_body_params = true;

        // Detect if body is a single Pydantic model → cache model_validate for direct call
        // Also detect plain dict body (no Pydantic, skip validation entirely)
        if (PyList_Check(body_params_obj) && PyList_GET_SIZE(body_params_obj) == 1) {
            PyObject* field_info = PyList_GET_ITEM(body_params_obj, 0);  // borrowed
            // field_info.field_info.annotation is the type
            PyObject* fi_attr = PyObject_GetAttrString(field_info, "field_info");
            if (fi_attr) {
                PyObject* annotation = PyObject_GetAttrString(fi_attr, "annotation");
                if (annotation) {
                    // Check if annotation is dict (plain dict body)
                    if (annotation == (PyObject*)&PyDict_Type) {
                        spec.body_is_plain_dict = true;
                    } else {
                        // Check if annotation has model_validate (Pydantic BaseModel)
                        PyObject* mv = PyObject_GetAttrString(annotation, "model_validate");
                        if (mv) {
                            spec.model_validate = mv;  // strong ref
                        } else {
                            PyErr_Clear();
                        }
                    }
                    Py_DECREF(annotation);
                }
                Py_DECREF(fi_attr);
            }
            PyErr_Clear();  // Clear any attribute errors
        }
    } else {
        spec.body_params = nullptr;
    }

    if (spec.body_param_name) spec.has_body_params = true;

    // Dependency injection
    spec.has_dependencies = false;
    spec.dependant = nullptr;
    spec.dep_solver = nullptr;
    if (dependant_obj != Py_None && dep_solver_obj != Py_None) {
        spec.has_dependencies = true;
        Py_INCREF(dependant_obj);
        spec.dependant = dependant_obj;
        Py_INCREF(dep_solver_obj);
        spec.dep_solver = dep_solver_obj;
    }

    route.fast_spec = std::move(spec);

    // Build O(1) lookup maps AFTER move — string_views must point into final location
    auto& final_spec = *route.fast_spec;
    for (size_t i = 0; i < final_spec.path_specs.size(); i++)
        final_spec.path_map[std::string_view(final_spec.path_specs[i].field_name)] = i;
    for (size_t i = 0; i < final_spec.query_specs.size(); i++) {
        const auto& fs = final_spec.query_specs[i];
        final_spec.query_map[std::string_view(fs.field_name)] = i;
        if (!fs.alias.empty() && fs.alias != fs.field_name)
            final_spec.query_map[std::string_view(fs.alias)] = i;
    }
    for (size_t i = 0; i < final_spec.header_specs.size(); i++)
        final_spec.header_map[std::string_view(final_spec.header_specs[i].header_lookup_key)] = i;
    for (size_t i = 0; i < final_spec.cookie_specs.size(); i++)
        final_spec.cookie_map[std::string_view(final_spec.cookie_specs[i].field_name)] = i;

    Py_RETURN_NONE;
}

// ── configure_cors ──────────────────────────────────────────────────────────

static PyObject* CoreApp_configure_cors(CoreAppObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "allow_origins", "allow_origin_regex", "allow_methods",
        "allow_headers", "allow_credentials", "expose_headers", "max_age", nullptr
    };

    PyObject* origins = Py_None;
    const char* origin_regex = nullptr;
    PyObject* methods = Py_None;
    PyObject* headers = Py_None;
    int credentials = 0;
    PyObject* expose = Py_None;
    long max_age = 600;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "|OzOOpOl", (char**)kwlist,
            &origins, &origin_regex, &methods, &headers,
            &credentials, &expose, &max_age)) {
        return nullptr;
    }

    auto config = std::make_shared<CorsConfig>();

    auto list_to_vec = [](PyObject* obj) -> std::vector<std::string> {
        std::vector<std::string> result;
        if (obj && obj != Py_None && PyList_Check(obj)) {
            Py_ssize_t n = PyList_GET_SIZE(obj);
            result.reserve(static_cast<size_t>(n));
            for (Py_ssize_t i = 0; i < n; i++) {
                const char* s = PyUnicode_AsUTF8(PyList_GET_ITEM(obj, i));
                if (s) result.emplace_back(s);
            }
        }
        return result;
    };

    config->allow_origins = list_to_vec(origins);
    config->allow_origin_regex = origin_regex ? std::optional<std::string>(origin_regex) : std::nullopt;
    config->allow_methods = list_to_vec(methods);
    config->allow_headers = list_to_vec(headers);
    config->allow_credentials = (bool)credentials;
    config->expose_headers = list_to_vec(expose);
    config->max_age = max_age;

    self->cors_config.store(config);

    Py_RETURN_NONE;
}

// ── configure_trusted_hosts ─────────────────────────────────────────────────

static PyObject* CoreApp_configure_trusted_hosts(CoreAppObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list of hosts");
        return nullptr;
    }
    auto config = std::make_shared<TrustedHostConfig>();
    Py_ssize_t n = PyList_GET_SIZE(arg);
    for (Py_ssize_t i = 0; i < n; i++) {
        const char* h = PyUnicode_AsUTF8(PyList_GET_ITEM(arg, i));
        if (h) config->allowed_hosts.emplace_back(h);
    }
    self->trusted_host_config.store(config);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_check_trusted_host(CoreAppObject* self, PyObject* arg) {
    const char* host = PyUnicode_AsUTF8(arg);
    if (!host) return nullptr;

    auto config = self->trusted_host_config.load();
    if (!config) Py_RETURN_TRUE;

    for (const auto& h : config->allowed_hosts) {
        if (h == "*" || h == host) Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// ── get_route_info ──────────────────────────────────────────────────────────

static PyObject* CoreApp_get_route_info(CoreAppObject* self, PyObject* arg) {
    Py_ssize_t idx = PyLong_AsSsize_t(arg);
    if (idx < 0 && PyErr_Occurred()) return nullptr;

    std::shared_lock lock(self->routes_mutex);
    if (idx < 0 || idx >= (Py_ssize_t)self->routes.size()) Py_RETURN_NONE;

    const auto& route = self->routes[idx];
    PyRef dict(PyDict_New());
    if (!dict) return nullptr;

    PyDict_SetItemString(dict.get(), "route_id", PyRef(PyLong_FromUnsignedLongLong(route.route_id)).get());
    PyDict_SetItemString(dict.get(), "status_code", PyRef(PyLong_FromLong(route.status_code)).get());
    PyDict_SetItemString(dict.get(), "is_coroutine", route.is_coroutine ? Py_True : Py_False);

    PyRef methods_list(PyList_New((Py_ssize_t)route.methods.size()));
    if (!methods_list) return nullptr;
    for (size_t i = 0; i < route.methods.size(); i++) {
        PyObject* m = PyUnicode_FromString(route.methods[i].c_str());
        if (!m) return nullptr;
        PyList_SET_ITEM(methods_list.get(), (Py_ssize_t)i, m);
    }
    PyDict_SetItemString(dict.get(), "methods", methods_list.get());

    PyRef tags_list(PyList_New((Py_ssize_t)route.tags.size()));
    if (!tags_list) return nullptr;
    for (size_t i = 0; i < route.tags.size(); i++) {
        PyObject* t = PyUnicode_FromString(route.tags[i].c_str());
        if (!t) return nullptr;
        PyList_SET_ITEM(tags_list.get(), (Py_ssize_t)i, t);
    }
    PyDict_SetItemString(dict.get(), "tags", tags_list.get());

    PyDict_SetItemString(dict.get(), "summary",
        route.summary ? PyUnicode_FromString(route.summary->c_str()) : Py_NewRef(Py_None));
    PyDict_SetItemString(dict.get(), "description",
        route.description ? PyUnicode_FromString(route.description->c_str()) : Py_NewRef(Py_None));
    PyDict_SetItemString(dict.get(), "operation_id",
        route.operation_id ? PyUnicode_FromString(route.operation_id->c_str()) : Py_NewRef(Py_None));

    return dict.release();
}

// ── add_exception_handler ───────────────────────────────────────────────────

static PyObject* CoreApp_add_exception_handler(CoreAppObject* self, PyObject* args) {
    int status_code;
    PyObject* handler;
    if (!PyArg_ParseTuple(args, "iO", &status_code, &handler)) return nullptr;

    Py_INCREF(handler);
    auto it = self->exception_handlers.find((uint16_t)status_code);
    if (it != self->exception_handlers.end()) {
        Py_DECREF(it->second);
        it->second = handler;
    } else {
        self->exception_handlers[(uint16_t)status_code] = handler;
    }
    Py_RETURN_NONE;
}

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP Response Builder — build raw HTTP/1.1 response as one byte buffer
// ═══════════════════════════════════════════════════════════════════════════════

static const char* status_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

// ── Pre-cached status lines: "HTTP/1.1 XXX Reason\r\n" ──────────────────────
// Eliminates status_reason() lookup + strlen() + per-byte copy on every response.
// NOTE: CachedStatusLine, CachedJsonPrefix, s_status_lines, s_json_prefixes
// are forward-declared near top of file (before build_static_response).

// Storage backing for cached strings (static lifetime)
static char s_status_storage[16384];
static size_t s_status_storage_used = 0;
static char s_json_prefix_storage[32768];
static size_t s_json_prefix_used = 0;

void init_status_line_cache() {
    struct Entry { int code; const char* reason; };
    static const Entry entries[] = {
        {200, "OK"}, {201, "Created"}, {204, "No Content"},
        {301, "Moved Permanently"}, {302, "Found"}, {304, "Not Modified"},
        {307, "Temporary Redirect"}, {308, "Permanent Redirect"},
        {400, "Bad Request"}, {401, "Unauthorized"}, {403, "Forbidden"},
        {404, "Not Found"}, {405, "Method Not Allowed"},
        {413, "Payload Too Large"}, {422, "Unprocessable Entity"},
        {500, "Internal Server Error"}, {502, "Bad Gateway"},
        {503, "Service Unavailable"},
    };

    memset(s_status_lines, 0, sizeof(s_status_lines));
    memset(s_json_prefixes, 0, sizeof(s_json_prefixes));

    for (const auto& e : entries) {
        if (e.code < 0 || e.code >= 600) continue;

        // Build "HTTP/1.1 XXX Reason\r\n"
        char* p = s_status_storage + s_status_storage_used;
        int n = snprintf(p, sizeof(s_status_storage) - s_status_storage_used,
                         "HTTP/1.1 %d %s\r\n", e.code, e.reason);
        if (n > 0 && s_status_storage_used + (size_t)n < sizeof(s_status_storage)) {
            s_status_lines[e.code] = {p, (size_t)n};
            s_status_storage_used += (size_t)n + 1;
        }

        // Build "HTTP/1.1 XXX Reason\r\ncontent-type: application/json\r\ncontent-length: "
        char* jp = s_json_prefix_storage + s_json_prefix_used;
        int jn = snprintf(jp, sizeof(s_json_prefix_storage) - s_json_prefix_used,
                          "HTTP/1.1 %d %s\r\ncontent-type: application/json\r\ncontent-length: ",
                          e.code, e.reason);
        if (jn > 0 && s_json_prefix_used + (size_t)jn < sizeof(s_json_prefix_storage)) {
            s_json_prefixes[e.code] = {jp, (size_t)jn};
            s_json_prefix_used += (size_t)jn + 1;
        }
    }
}

// ── CORS helpers ──────────────────────────────────────────────────────────

// Check if origin is allowed by CORS config. Returns true if allowed.
static bool cors_origin_allowed(const CorsConfig* cors, const char* origin, size_t origin_len) {
    if (!cors || !origin || origin_len == 0) return false;
    std::string_view origin_sv(origin, origin_len);
    for (const auto& o : cors->allow_origins) {
        if (o == "*" || std::string_view(o) == origin_sv) return true;
    }
    return false;
}

// Check for CRLF injection in origin strings
static inline bool contains_crlf(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n') return true;
    }
    return false;
}

// Build CORS headers string into a buffer. Returns number of bytes written.
static size_t build_cors_headers(std::vector<char>& buf, const CorsConfig* cors,
                                  const char* origin, size_t origin_len) {
    if (!cors || !origin || origin_len == 0) return 0;
    if (contains_crlf(origin, origin_len)) return 0;
    size_t start = buf.size();

    // Pre-reserve for typical CORS headers to avoid reallocations
    size_t estimate = 200 + origin_len;
    for (const auto& h : cors->expose_headers) estimate += h.size() + 2;
    buf.reserve(buf.size() + estimate);

    // Access-Control-Allow-Origin
    static const char ACAO[] = "\r\naccess-control-allow-origin: ";
    buf.insert(buf.end(), ACAO, ACAO + sizeof(ACAO) - 1);
    // If wildcard and no credentials, use *; otherwise echo origin
    if (!cors->allow_credentials && cors->allow_origins.size() == 1 && cors->allow_origins[0] == "*") {
        buf.push_back('*');
    } else {
        buf.insert(buf.end(), origin, origin + origin_len);
    }

    // Access-Control-Allow-Credentials
    if (cors->allow_credentials) {
        static const char ACAC[] = "\r\naccess-control-allow-credentials: true";
        buf.insert(buf.end(), ACAC, ACAC + sizeof(ACAC) - 1);
    }

    // Access-Control-Expose-Headers
    if (!cors->expose_headers.empty()) {
        static const char ACEH[] = "\r\naccess-control-expose-headers: ";
        buf.insert(buf.end(), ACEH, ACEH + sizeof(ACEH) - 1);
        for (size_t i = 0; i < cors->expose_headers.size(); i++) {
            if (i > 0) { buf.push_back(','); buf.push_back(' '); }
            const auto& h = cors->expose_headers[i];
            buf.insert(buf.end(), h.begin(), h.end());
        }
    }

    // Vary: Origin (needed when not wildcard)
    if (cors->allow_credentials || cors->allow_origins.size() > 1 ||
        (cors->allow_origins.size() == 1 && cors->allow_origins[0] != "*")) {
        static const char VARY[] = "\r\nvary: Origin";
        buf.insert(buf.end(), VARY, VARY + sizeof(VARY) - 1);
    }

    return buf.size() - start;
}

// Build a full CORS preflight (OPTIONS) response
static PyObject* build_cors_preflight_response(
    const CorsConfig* cors, const char* origin, size_t origin_len, bool keep_alive)
{
    if (contains_crlf(origin, origin_len)) Py_RETURN_NONE;

    auto buf = acquire_buffer();
    // Pre-reserve for preflight response
    size_t estimate = 512;
    for (const auto& m : cors->allow_methods) estimate += m.size() + 2;
    for (const auto& h : cors->allow_headers) estimate += h.size() + 2;
    estimate += origin_len;
    buf.reserve(estimate);

    static const char STATUS[] = "HTTP/1.1 204 No Content\r\naccess-control-allow-origin: ";
    buf.insert(buf.end(), STATUS, STATUS + sizeof(STATUS) - 1);

    // Origin
    if (!cors->allow_credentials && cors->allow_origins.size() == 1 && cors->allow_origins[0] == "*") {
        buf.push_back('*');
    } else {
        buf.insert(buf.end(), origin, origin + origin_len);
    }

    // Allow-Methods
    static const char ACAM[] = "\r\naccess-control-allow-methods: ";
    buf.insert(buf.end(), ACAM, ACAM + sizeof(ACAM) - 1);
    if (cors->allow_methods.empty()) {
        static const char DEF_METHODS[] = "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD";
        buf.insert(buf.end(), DEF_METHODS, DEF_METHODS + sizeof(DEF_METHODS) - 1);
    } else {
        for (size_t i = 0; i < cors->allow_methods.size(); i++) {
            if (i > 0) { buf.push_back(','); buf.push_back(' '); }
            const auto& m = cors->allow_methods[i];
            buf.insert(buf.end(), m.begin(), m.end());
        }
    }

    // Allow-Headers
    if (!cors->allow_headers.empty()) {
        static const char ACAH[] = "\r\naccess-control-allow-headers: ";
        buf.insert(buf.end(), ACAH, ACAH + sizeof(ACAH) - 1);
        for (size_t i = 0; i < cors->allow_headers.size(); i++) {
            if (i > 0) { buf.push_back(','); buf.push_back(' '); }
            const auto& h = cors->allow_headers[i];
            buf.insert(buf.end(), h.begin(), h.end());
        }
    }

    // Max-Age
    static const char AMA[] = "\r\naccess-control-max-age: ";
    buf.insert(buf.end(), AMA, AMA + sizeof(AMA) - 1);
    char age_buf[16];
    int age_n = fast_i64_to_buf(age_buf, cors->max_age);
    buf.insert(buf.end(), age_buf, age_buf + age_n);

    // Credentials
    if (cors->allow_credentials) {
        static const char ACAC[] = "\r\naccess-control-allow-credentials: true";
        buf.insert(buf.end(), ACAC, ACAC + sizeof(ACAC) - 1);
    }

    // Vary
    static const char VARY[] = "\r\nvary: Origin";
    buf.insert(buf.end(), VARY, VARY + sizeof(VARY) - 1);

    // Content-Length: 0 + Connection
    static const char CL0_KA[] = "\r\ncontent-length: 0\r\nconnection: keep-alive\r\n\r\n";
    static const char CL0_CLOSE[] = "\r\ncontent-length: 0\r\nconnection: close\r\n\r\n";
    if (keep_alive) {
        buf.insert(buf.end(), CL0_KA, CL0_KA + sizeof(CL0_KA) - 1);
    } else {
        buf.insert(buf.end(), CL0_CLOSE, CL0_CLOSE + sizeof(CL0_CLOSE) - 1);
    }

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// ── Trusted host check ──────────────────────────────────────────────────────

static bool check_trusted_host_inline(const TrustedHostConfig* th, const char* host, size_t host_len) {
    if (!th) return true;  // No config = allow all
    std::string_view host_sv(host, host_len);
    // Strip port if present
    auto colon = host_sv.find(':');
    if (colon != std::string_view::npos) {
        host_sv = host_sv.substr(0, colon);
    }
    for (const auto& h : th->allowed_hosts) {
        if (h == "*" || std::string_view(h) == host_sv) return true;
    }
    return false;
}

// ── Inline compression (no Python overhead — raw zlib/brotli) ────────────────
// Returns encoding name ("gzip" or "br") if compressed, nullptr if not.
// Caller provides pre-allocated `out` vector; body is replaced only if compressed
// is smaller than original.

static const char* try_compress_inline(
    const char* body, size_t body_len,
    const char* ae, size_t ae_len,
    std::vector<char>& out)
{
    if (body_len < 500) return nullptr;

    // Quick scan for accepted encodings — SSO handles ≤22 bytes without heap alloc
    std::string ae_lower(ae, ae_len);
    for (auto& c : ae_lower) if (c >= 'A' && c <= 'Z') c += 32;
    bool accept_br = false, accept_gzip = false;
#if HAS_BROTLI
    accept_br = ae_lower.find("br") != std::string::npos;
#endif
    accept_gzip = ae_lower.find("gzip") != std::string::npos;

    if (!accept_br && !accept_gzip) return nullptr;

    // GIL release: compression only touches raw C data, no Python objects
    const char* result = nullptr;

    Py_BEGIN_ALLOW_THREADS

#if HAS_BROTLI
    if (accept_br) {
        size_t output_size = BrotliEncoderMaxCompressedSize(body_len);
        if (output_size == 0) output_size = body_len + 1024;
        out.resize(output_size);
        int ok = BrotliEncoderCompress(
            4, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
            body_len, (const uint8_t*)body,
            &output_size, (uint8_t*)out.data());
        if (ok && output_size < body_len) {
            out.resize(output_size);
            result = "br";
        }
    }
#endif

    if (!result && accept_gzip) {
        // Size-adaptive compression: level 1 for <4KB (speed), 4 for 4-64KB, 6 for >64KB
        int gz_level = (body_len < 4096) ? 1 : (body_len <= 65536) ? 4 : 6;
        z_stream strm = {};
        int ret = deflateInit2(&strm, gz_level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
        if (ret == Z_OK) {
            out.resize(deflateBound(&strm, (uLong)body_len));
            strm.next_in = (Bytef*)body;
            strm.avail_in = (uInt)body_len;
            strm.next_out = (Bytef*)out.data();
            strm.avail_out = (uInt)out.size();
            ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_END && strm.total_out < body_len) {
                out.resize(strm.total_out);
                deflateEnd(&strm);
                result = "gzip";
            } else {
                deflateEnd(&strm);
            }
        }
    }

    Py_END_ALLOW_THREADS

    return result;
}

// ── Response builder with optional CORS + compression headers ────────────────

static PyObject* build_http_response_bytes(
    int status_code, const char* body_data, size_t body_len, bool keep_alive,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0,
    const char* content_encoding = nullptr)
{
    // Pre-built header templates — single memcpy for common case
    static const char HDR_200_JSON[] =
        "HTTP/1.1 200 OK\r\n"
        "content-type: application/json\r\n"
        "content-length: ";
    static const char CONN_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
    static const char CONN_CLOSE[] = "\r\nconnection: close\r\n\r\n";

    auto buf = acquire_buffer();

    // Reserve total size upfront — eliminates ALL reallocations
    buf.reserve(256 + body_len);

    if (status_code == 200 && !cors && !content_encoding && keep_alive) {
        // Ultra-fast path: single resize + 4 memcpy for 200 JSON keep-alive
        // (eliminates multiple buf.insert calls + branch overhead)
        char cl_buf[20];
        int cl_len = fast_i64_to_buf(cl_buf, (long long)body_len);
        static constexpr char SUFFIX_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
        size_t total = sizeof(HDR_200_JSON) - 1 + cl_len + sizeof(SUFFIX_KA) - 1 + body_len;
        buf.resize(total);
        char* dst = buf.data();
        memcpy(dst, HDR_200_JSON, sizeof(HDR_200_JSON) - 1); dst += sizeof(HDR_200_JSON) - 1;
        memcpy(dst, cl_buf, cl_len); dst += cl_len;
        memcpy(dst, SUFFIX_KA, sizeof(SUFFIX_KA) - 1); dst += sizeof(SUFFIX_KA) - 1;
        memcpy(dst, body_data, body_len);
        PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
        release_buffer(std::move(buf));
        return result;
    }

    if (status_code == 200 && !cors && !content_encoding) {
        buf.insert(buf.end(), HDR_200_JSON, HDR_200_JSON + sizeof(HDR_200_JSON) - 1);
    } else if (status_code == 200) {
        // 200 with CORS or compression
        buf.insert(buf.end(), HDR_200_JSON, HDR_200_JSON + sizeof(HDR_200_JSON) - 1);
    } else if (status_code > 0 && status_code < 600 && s_json_prefixes[status_code].data) {
        // Cached JSON prefix: "HTTP/1.1 XXX Reason\r\ncontent-type: application/json\r\ncontent-length: "
        const auto& jp = s_json_prefixes[status_code];
        buf.insert(buf.end(), jp.data, jp.data + jp.len);
    } else {
        // Uncached status code — build dynamically
        static const char prefix[] = "HTTP/1.1 ";
        buf.insert(buf.end(), prefix, prefix + sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf.insert(buf.end(), sc_buf, sc_buf + sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf.insert(buf.end(), reason, reason + rlen);
        static const char ct_hdr[] = "\r\ncontent-type: application/json\r\ncontent-length: ";
        buf.insert(buf.end(), ct_hdr, ct_hdr + sizeof(ct_hdr) - 1);
    }

    // Content-Length value
    char sc[20];
    int n = fast_i64_to_buf(sc, (long long)body_len);
    buf.insert(buf.end(), sc, sc + n);

    // Content-Encoding header (when compressed)
    if (content_encoding) {
        static const char ce_prefix[] = "\r\ncontent-encoding: ";
        buf.insert(buf.end(), ce_prefix, ce_prefix + sizeof(ce_prefix) - 1);
        size_t ce_len = strlen(content_encoding);
        buf.insert(buf.end(), content_encoding, content_encoding + ce_len);
        // Also add Vary: Accept-Encoding for caching
        static const char vary_hdr[] = "\r\nvary: Accept-Encoding";
        buf.insert(buf.end(), vary_hdr, vary_hdr + sizeof(vary_hdr) - 1);
    }

    // CORS headers (before connection header)
    if (cors && origin && origin_len > 0) {
        build_cors_headers(buf, cors, origin, origin_len);
    }

    // Connection header + end-of-headers (single insert)
    if (keep_alive) {
        buf.insert(buf.end(), CONN_KA, CONN_KA + sizeof(CONN_KA) - 1);
    } else {
        buf.insert(buf.end(), CONN_CLOSE, CONN_CLOSE + sizeof(CONN_CLOSE) - 1);
    }

    // Body
    buf.insert(buf.end(), body_data, body_data + body_len);

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// Escape a C string for safe JSON embedding (handles " \ and control chars)
static void json_escape_cstr(std::vector<char>& buf, const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { buf.push_back('\\'); buf.push_back('"'); }
        else if (c == '\\') { buf.push_back('\\'); buf.push_back('\\'); }
        else if (c == '\n') { buf.push_back('\\'); buf.push_back('n'); }
        else if (c == '\r') { buf.push_back('\\'); buf.push_back('r'); }
        else if (c == '\t') { buf.push_back('\\'); buf.push_back('t'); }
        else if (c < 0x20) {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            buf.insert(buf.end(), esc, esc + 6);
        }
        else { buf.push_back((char)c); }
    }
}

static PyObject* build_http_error_response(int status_code, const char* message, bool keep_alive,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0) {
    // Build JSON error body: {"detail":"message"} with proper escaping
    auto body_buf = acquire_buffer();
    const char* pre = "{\"detail\":\"";
    body_buf.insert(body_buf.end(), pre, pre + 11);
    json_escape_cstr(body_buf, message, strlen(message));
    const char* post = "\"}";
    body_buf.insert(body_buf.end(), post, post + 2);

    PyObject* result = build_http_response_bytes(
        status_code, body_buf.data(), body_buf.size(), keep_alive, cors, origin, origin_len);
    release_buffer(std::move(body_buf));
    return result;
}


static int write_to_transport(PyObject* transport, PyObject* data) {
    if (!transport || transport == Py_None) return -1;

    if (!g_str_write) g_str_write = PyUnicode_InternFromString("write");

    // Skip is_closing() check — direct write is faster and transport.write()
    // handles closed transport gracefully. Saves one Python method call per request.
    PyRef result(PyObject_CallMethodOneArg(transport, g_str_write, data));
    if (!result) {
        log_and_clear_pyerr("transport.write() failed");
        return -1;
    }
    return 0;
}

// ── HttpConnectionBuffer — replaces Python bytearray + memmove ──────────────
// Linear buffer with read/write offsets. Compact only when read_pos > 50% capacity.
// Eliminates: Python memoryview creation, slice ops, O(N) memmove per request.

class HttpConnectionBuffer {
    static constexpr size_t INITIAL_CAPACITY = 8192;
    static constexpr size_t MAX_CAPACITY = 1048576;  // 1MB

    uint8_t* buf_;
    size_t capacity_;
    size_t read_pos_;
    size_t write_pos_;

public:
    HttpConnectionBuffer()
        : buf_(static_cast<uint8_t*>(malloc(INITIAL_CAPACITY)))
        , capacity_(INITIAL_CAPACITY)
        , read_pos_(0)
        , write_pos_(0) {}

    ~HttpConnectionBuffer() { free(buf_); }

    bool append(const uint8_t* data, size_t len) {
        size_t used = write_pos_ - read_pos_;
        size_t needed = used + len;
        if (needed > MAX_CAPACITY) return false;

        // Compact if read_pos > 50% of capacity (amortized memmove)
        if (read_pos_ > capacity_ / 2) {
            memmove(buf_, buf_ + read_pos_, used);
            write_pos_ = used;
            read_pos_ = 0;
        }

        // Grow if needed
        if (write_pos_ + len > capacity_) {
            size_t new_cap = capacity_;
            while (new_cap < write_pos_ + len) new_cap *= 2;
            if (new_cap > MAX_CAPACITY) new_cap = MAX_CAPACITY;
            uint8_t* nb = static_cast<uint8_t*>(realloc(buf_, new_cap));
            if (!nb) return false;
            buf_ = nb;
            capacity_ = new_cap;
        }

        memcpy(buf_ + write_pos_, data, len);
        write_pos_ += len;
        return true;
    }

    const uint8_t* data() const { return buf_ + read_pos_; }
    size_t size() const { return write_pos_ - read_pos_; }

    void consume(size_t n) {
        read_pos_ += n;
        if (read_pos_ >= write_pos_) {
            read_pos_ = 0;
            write_pos_ = 0;
        }
    }

    void clear() {
        read_pos_ = 0;
        write_pos_ = 0;
        // Shrink if capacity grew beyond 4x initial to reclaim memory
        if (capacity_ > INITIAL_CAPACITY * 4) {
            uint8_t* nb = static_cast<uint8_t*>(realloc(buf_, INITIAL_CAPACITY));
            if (nb) {
                buf_ = nb;
                capacity_ = INITIAL_CAPACITY;
            }
        }
    }
};

static const char* HTTP_BUF_CAPSULE_NAME = "http_connection_buffer";

namespace {
void http_buf_destructor(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME);
    if (ptr) delete static_cast<HttpConnectionBuffer*>(ptr);
}
}

PyObject* py_http_buf_create(PyObject* /*self*/, PyObject* /*args*/) {
    auto* buf = new HttpConnectionBuffer();
    return PyCapsule_New(buf, HTTP_BUF_CAPSULE_NAME, http_buf_destructor);
}

// http_buf_append: Append data to buffer. Returns True on success, False on overflow.
PyObject* py_http_buf_append(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;
    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) return nullptr;

    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }

    bool ok = buf->append(static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
    PyBuffer_Release(&py_buf);

    if (ok) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// http_buf_get_view: Return a memoryview into the readable portion of the buffer.
// Zero-copy — the memoryview points directly into the C++ buffer.
PyObject* py_http_buf_get_view(PyObject* /*self*/, PyObject* capsule) {
    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }
    if (buf->size() == 0) Py_RETURN_NONE;
    return PyMemoryView_FromMemory(
        (char*)buf->data(), (Py_ssize_t)buf->size(), PyBUF_READ);
}

// http_buf_consume: Advance read position by N bytes. O(1) — no memmove!
PyObject* py_http_buf_consume(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_ssize_t n;
    if (!PyArg_ParseTuple(args, "On", &capsule, &n)) return nullptr;

    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }
    buf->consume((size_t)n);
    Py_RETURN_NONE;
}

// http_buf_clear: Reset buffer to empty state.
PyObject* py_http_buf_clear(PyObject* /*self*/, PyObject* capsule) {
    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }
    buf->clear();
    Py_RETURN_NONE;
}

// http_buf_len: Return readable byte count.
PyObject* py_http_buf_len(PyObject* /*self*/, PyObject* capsule) {
    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }
    return PyLong_FromSsize_t((Py_ssize_t)buf->size());
}

// ── Post-response hook helper (for logging middleware) ──────────────────────
// Calls the registered Python hook with (method, path, status_code, duration_ms)
static inline void fire_post_response_hook(
    CoreAppObject* self, const char* method_data, size_t method_len,
    const char* path_data, size_t path_len, int status_code,
    std::chrono::steady_clock::time_point start_time)
{
    if (!self->post_response_hook) return;
    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start_time).count();
    PyRef m(PyUnicode_FromStringAndSize(method_data, (Py_ssize_t)method_len));
    PyRef p(PyUnicode_FromStringAndSize(path_data, (Py_ssize_t)path_len));
    PyRef sc(PyLong_FromLong(status_code));
    PyRef dur(PyFloat_FromDouble(ms));
    if (m && p && sc && dur) {
        PyRef r(PyObject_CallFunctionObjArgs(self->post_response_hook,
                m.get(), p.get(), sc.get(), dur.get(), nullptr));
        if (!r) log_and_clear_pyerr("post_response_hook error");
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// handle_http — C++ HTTP Server Hot Path (METH_FASTCALL)
//
// Called from Python asyncio Protocol's data_received().
// args[0] = bytes (accumulated buffer from socket)
// args[1] = transport (asyncio Transport — for transport.write())
//
// Returns:
//   (consumed_bytes, True)      — sync endpoint handled, response written
//   (consumed_bytes, coroutine) — async endpoint, Python must await
//   (0, None)                   — need more data (incomplete HTTP request)
//   (-1, None)                  — parse error (400 already sent)
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_handle_http(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs < 2 || nargs > 3) {
        PyErr_SetString(PyExc_TypeError, "handle_http requires 2-3 args (buffer, transport[, offset])");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];
    PyObject* transport = args[1];
    // OPT-7: Optional offset parameter eliminates memoryview slicing in Python loop
    Py_ssize_t offset = 0;
    if (nargs == 3) {
        offset = PyLong_AsSsize_t(args[2]);
        if (offset < 0 && PyErr_Occurred()) return nullptr;
    }

    // Accept any buffer protocol object (bytes, bytearray, memoryview)
    Py_buffer view;
    if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) {
        return nullptr;
    }
    // RAII guard to release buffer on all return paths
    struct BufferGuard {
        Py_buffer* v;
        ~BufferGuard() { PyBuffer_Release(v); }
    } buf_guard{&view};

    // Apply offset (OPT-7: avoids Python memoryview slice creation per request)
    if (offset > view.len) offset = view.len;
    char* buf_data = (char*)view.buf + offset;
    Py_ssize_t buf_len = view.len - offset;

    // ── Parse HTTP request ───────────────────────────────────────────────
    ParsedHttpRequest req = {};
    int parse_result = parse_http_request(buf_data, (size_t)buf_len, &req);

    if (parse_result == 0) {
        // Need more data
        PyRef zero(PyLong_FromLong(0));
        Py_INCREF(Py_None);
        return PyTuple_Pack(2, zero.get(), Py_None);
    }

    if (parse_result < 0) {
        // Parse error — send 400
        PyRef err_resp(build_http_error_response(400, "Bad Request", false));
        if (err_resp) write_to_transport(transport, err_resp.get());
        PyRef neg(PyLong_FromLong(-1));
        Py_INCREF(Py_None);
        return PyTuple_Pack(2, neg.get(), Py_None);
    }

    self->total_requests.fetch_add(1, std::memory_order_relaxed);
    self->active_requests.fetch_add(1, std::memory_order_relaxed);

    // Capture start time for post-response hook (logging middleware)
    auto request_start_time = std::chrono::steady_clock::now();

    // ── Extract Origin, Host, Accept-Encoding, Authorization headers ────
    StringView origin_sv;
    StringView host_sv;
    StringView accept_encoding_sv;
    StringView content_type_sv;
    StringView authorization_sv;
    {
        // Only look for origin if CORS is configured (skip if not)
        bool want_origin = (self->cors_config.load().get() != nullptr);
        int found = 0;
        const int need = want_origin ? 5 : 4;
        for (int i = 0; i < req.header_count && found < need; i++) {
            const auto& hdr = req.headers[i];
            size_t nlen = hdr.name.len;
            char fb = nlen > 0 ? (hdr.name.data[0] | 0x20) : 0;
            switch (nlen) {
                case 4:
                    if (host_sv.empty() && fb == 'h' && hdr.name.iequals("host", 4))
                        { host_sv = hdr.value; found++; }
                    break;
                case 6:
                    if (want_origin && origin_sv.empty() && fb == 'o' && hdr.name.iequals("origin", 6))
                        { origin_sv = hdr.value; found++; }
                    break;
                case 12:
                    if (content_type_sv.empty() && fb == 'c' && hdr.name.iequals("content-type", 12))
                        { content_type_sv = hdr.value; found++; }
                    break;
                case 13:
                    if (authorization_sv.empty() && fb == 'a' && hdr.name.iequals("authorization", 13))
                        { authorization_sv = hdr.value; found++; }
                    break;
                case 15:
                    if (accept_encoding_sv.empty() && fb == 'a' && hdr.name.iequals("accept-encoding", 15))
                        { accept_encoding_sv = hdr.value; found++; }
                    break;
            }
        }
    }

    // ── Trusted host check ──────────────────────────────────────────────
    auto th_config = self->trusted_host_config.load();
    if (th_config && !host_sv.empty()) {
        if (!check_trusted_host_inline(th_config.get(), host_sv.data, host_sv.len)) {
            self->active_requests.fetch_sub(1, std::memory_order_release);
            PyRef resp(build_http_error_response(400, "Invalid host header", req.keep_alive));
            if (resp) write_to_transport(transport, resp.get());
            return make_consumed_true(req.total_consumed);
        }
    }

    // ── CORS preflight (OPTIONS with Origin) ────────────────────────────
    auto cors = self->cors_config.load();
    const CorsConfig* cors_ptr = cors.get();
    bool has_cors = cors_ptr && !origin_sv.empty() && cors_origin_allowed(cors_ptr, origin_sv.data, origin_sv.len);

    if (has_cors && req.method.len == 7 && memcmp(req.method.data, "OPTIONS", 7) == 0) {
        // Full CORS preflight response — entirely in C++, no route needed
        self->active_requests.fetch_sub(1, std::memory_order_release);
        PyRef resp(build_cors_preflight_response(cors_ptr, origin_sv.data, origin_sv.len, req.keep_alive));
        if (resp) write_to_transport(transport, resp.get());
        return make_consumed_true(req.total_consumed);
    }

    // ── Serve /openapi.json, /docs, /redoc (pre-built responses) ──────
    if (req.path.len > 0) {
        std::string_view path_sv(req.path.data, req.path.len);
        if (self->openapi_json_resp && path_sv == self->openapi_url) {
            self->active_requests.fetch_sub(1, std::memory_order_release);
            write_to_transport(transport, self->openapi_json_resp);
            return make_consumed_true(req.total_consumed);
        }
        if (self->docs_html_resp && path_sv == self->docs_url) {
            self->active_requests.fetch_sub(1, std::memory_order_release);
            write_to_transport(transport, self->docs_html_resp);
            return make_consumed_true(req.total_consumed);
        }
        if (self->redoc_html_resp && path_sv == self->redoc_url) {
            self->active_requests.fetch_sub(1, std::memory_order_release);
            write_to_transport(transport, self->redoc_html_resp);
            return make_consumed_true(req.total_consumed);
        }
    }

    // ── WebSocket upgrade detection ────────────────────────────────────
    if (req.upgrade) {
        // Extract Sec-WebSocket-Key header
        StringView ws_key;
        for (int i = 0; i < req.header_count; i++) {
            if (req.headers[i].name.iequals("sec-websocket-key", 17)) {
                ws_key = req.headers[i].value;
                break;
            }
        }

        if (ws_key.len > 0) {
            // Build and send 101 Switching Protocols
            auto upgrade_resp = ws_build_upgrade_response(ws_key.data, ws_key.len);
            PyRef resp_bytes(PyBytes_FromStringAndSize(upgrade_resp.data(), (Py_ssize_t)upgrade_resp.size()));
            if (resp_bytes) write_to_transport(transport, resp_bytes.get());

            // Find the WebSocket route endpoint
            std::shared_lock ws_lock(self->routes_mutex);
            auto ws_match = self->router.at(req.path.data, req.path.len);
            PyObject* ws_endpoint = nullptr;
            if (ws_match) {
                int ws_idx = ws_match->route_index;
                if (ws_idx >= 0 && ws_idx < (int)self->routes.size()) {
                    ws_endpoint = self->routes[ws_idx].endpoint;
                    Py_INCREF(ws_endpoint);
                }
            }
            ws_lock.unlock();

            self->active_requests.fetch_sub(1, std::memory_order_release);
            PyRef consumed(PyLong_FromLongLong((long long)req.total_consumed));

            if (ws_endpoint) {
                // Return (consumed, ("ws", endpoint, path_params_dict, path, headers_list, query_bytes)) for Python to handle
                PyRef ws_tag(PyUnicode_InternFromString("ws"));
                PyRef path_params(PyDict_New());
                if (ws_match && ws_match->param_count > 0) {
                    for (int pi = 0; pi < ws_match->param_count; pi++) {
                        auto k = ws_match->params[pi].name;
                        auto v = ws_match->params[pi].value;
                        PyRef pk(PyUnicode_FromStringAndSize(k.data(), k.size()));
                        PyRef pv(PyUnicode_FromStringAndSize(v.data(), v.size()));
                        if (pk && pv) PyDict_SetItem(path_params.get(), pk.get(), pv.get());
                    }
                }
                PyRef path_str(PyUnicode_FromStringAndSize(req.path.data, (Py_ssize_t)req.path.len));

                // Build ASGI-style headers list: [(name_bytes, value_bytes), ...]
                PyRef headers_list(PyList_New(req.header_count));
                if (headers_list) {
                    for (int hi = 0; hi < req.header_count; hi++) {
                        const auto& hdr = req.headers[hi];
                        PyRef hname(PyBytes_FromStringAndSize(hdr.name.data, (Py_ssize_t)hdr.name.len));
                        PyRef hval(PyBytes_FromStringAndSize(hdr.value.data, (Py_ssize_t)hdr.value.len));
                        if (hname && hval) {
                            PyObject* pair = PyTuple_Pack(2, hname.get(), hval.get());
                            PyList_SET_ITEM(headers_list.get(), hi, pair);  // steals ref
                        } else {
                            Py_INCREF(Py_None);
                            PyList_SET_ITEM(headers_list.get(), hi, Py_None);
                        }
                    }
                }

                // Query string as bytes
                PyRef qs_bytes(PyBytes_FromStringAndSize(
                    req.query_string.data ? req.query_string.data : "",
                    req.query_string.data ? (Py_ssize_t)req.query_string.len : 0));

                PyRef ws_info(PyTuple_Pack(6, ws_tag.get(), ws_endpoint, path_params.get(),
                              path_str.get(), headers_list.get(), qs_bytes.get()));
                Py_DECREF(ws_endpoint);
                return PyTuple_Pack(2, consumed.get(), ws_info.get());
            }

            // No route found for WebSocket
            PyRef close_frame_bytes(PyBytes_FromStringAndSize(nullptr, 0));
            Py_INCREF(Py_True);
            return PyTuple_Pack(2, consumed.get(), Py_True);
        }
    }

    // ── Route matching ───────────────────────────────────────────────────
    // Skip lock when routes are frozen (after startup) — atomic check first.
    // Auto-freeze on first request if not already frozen to eliminate lock contention.
    bool rt_frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::shared_lock lock(self->routes_mutex, std::defer_lock);
    if (!rt_frozen) {
        // Try to auto-freeze: first request triggers freeze
        std::unique_lock wlock(self->routes_mutex);
        if (!self->routes_frozen.load(std::memory_order_relaxed)) {
            self->routes_frozen.store(true, std::memory_order_release);
        }
        rt_frozen = true;
        // wlock released here — routes are now frozen, no lock needed going forward
    }

    auto match = self->router.at(req.path.data, req.path.len);
    if (!match) {
        // ── Trailing slash redirect: try with/without '/' ───────────
        std::string alt_path(req.path.data, req.path.len);
        if (!alt_path.empty() && alt_path.back() == '/') {
            alt_path.pop_back();  // try without trailing slash
        } else {
            alt_path.push_back('/');  // try with trailing slash
        }
        auto alt_match = self->router.at(alt_path.c_str(), alt_path.size());
        if (alt_match) {
            if (!rt_frozen) lock.unlock();
            self->active_requests.fetch_sub(1, std::memory_order_release);
            // Build 307 redirect response
            auto buf = acquire_buffer();
            buf.reserve(256);
            static const char redir[] = "HTTP/1.1 307 Temporary Redirect\r\nlocation: ";
            buf.insert(buf.end(), redir, redir + sizeof(redir) - 1);
            buf.insert(buf.end(), alt_path.begin(), alt_path.end());
            // Add query string if present
            if (!req.query_string.empty()) {
                buf.push_back('?');
                buf.insert(buf.end(), req.query_string.data, req.query_string.data + req.query_string.len);
            }
            static const char redir_end[] = "\r\ncontent-length: 0\r\nconnection: keep-alive\r\n\r\n";
            buf.insert(buf.end(), redir_end, redir_end + sizeof(redir_end) - 1);
            PyRef resp(PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size()));
            release_buffer(std::move(buf));
            if (resp) write_to_transport(transport, resp.get());
            return make_consumed_true(req.total_consumed);
        }

        if (!rt_frozen) lock.unlock();
        self->active_requests.fetch_sub(1, std::memory_order_release);
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_to_transport(transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 404, request_start_time);
        return make_consumed_true(req.total_consumed);
    }

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) {
        if (!rt_frozen) lock.unlock();
        self->active_requests.fetch_sub(1, std::memory_order_release);
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_to_transport(transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 404, request_start_time);
        return make_consumed_true(req.total_consumed);
    }

    const auto& route = self->routes[idx];

    // ── Method check (O(1) bitmask) ────────────────────────────────────
    if (route.method_mask) {
        uint8_t req_method = method_str_to_bit(req.method.data, req.method.len);
        if (!(route.method_mask & req_method)) {
            if (!rt_frozen) lock.unlock();
            self->active_requests.fetch_sub(1, std::memory_order_release);
            PyRef resp(build_http_error_response(405, "Method Not Allowed", req.keep_alive,
                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
            if (resp) write_to_transport(transport, resp.get());
            fire_post_response_hook(self, req.method.data, req.method.len,
                                    req.path.data, req.path.len, 405, request_start_time);
            return make_consumed_true(req.total_consumed);
        }
    }

    // ── Rate limiting (C++ native, per client IP) — sharded for low contention
    if (self->rate_limit_enabled && !self->current_client_ip.empty()) {
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t window_ns = (int64_t)self->rate_limit_window_seconds * 1'000'000'000LL;
        size_t shard_idx = std::hash<std::string>{}(self->current_client_ip) % RATE_LIMIT_SHARDS;
        auto& shard = self->rate_limit_shards[shard_idx];
        std::lock_guard<std::mutex> rl_lock(shard.mutex);
        auto& entry = shard.counters[self->current_client_ip];
        if (now_ns - entry.window_start_ns > window_ns) {
            entry.count = 1;
            entry.window_start_ns = now_ns;
        } else {
            entry.count++;
        }
        if (entry.count > self->rate_limit_max_requests) {
            // Unlock route lock before writing response
            if (!rt_frozen) lock.unlock();
            self->active_requests.fetch_sub(1, std::memory_order_release);
            PyRef resp(build_http_error_response(429, "Rate limit exceeded", req.keep_alive,
                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
            if (resp) write_to_transport(transport, resp.get());
            return make_consumed_true(req.total_consumed);
        }
        // Periodic cleanup: sweep stale entries every 10K requests
        static thread_local uint32_t rl_cleanup_counter = 0;
        if (++rl_cleanup_counter >= 10000) {
            rl_cleanup_counter = 0;
            for (auto it = shard.counters.begin(); it != shard.counters.end(); ) {
                if (now_ns - it->second.window_start_ns > 2 * window_ns)
                    it = shard.counters.erase(it);
                else
                    ++it;
            }
        }
    }

    if (!route.fast_spec) {
        if (!rt_frozen) lock.unlock();
        self->active_requests.fetch_sub(1, std::memory_order_release);
        PyRef resp(build_http_error_response(500, "Route not configured", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_to_transport(transport, resp.get());
        return make_consumed_true(req.total_consumed);
    }

    // Copy route data to locals and release lock early.
    // Routes are immutable after startup — safe to use spec pointer after unlock.
    const FastRouteSpec* spec_ptr = &(*route.fast_spec);
    PyObject* endpoint_local = route.endpoint;
    Py_INCREF(endpoint_local);
    uint16_t status_code_local = route.status_code;
    bool has_body_params_local = spec_ptr->has_body_params;
    PyObject* body_params_local = spec_ptr->body_params;
    if (body_params_local) Py_INCREF(body_params_local);
    bool embed_body_local = spec_ptr->embed_body_fields;
    PyObject* response_model_local = route.response_model_field;
    if (response_model_local) Py_INCREF(response_model_local);  // strong ref
    bool is_form_local = route.is_form;
    bool is_coro_local = route.is_coroutine;

    if (!rt_frozen) lock.unlock();

    // RAII guard — auto-DECREF response_model_local on any return path
    struct PyObjGuard { PyObject* p; ~PyObjGuard() { Py_XDECREF(p); } };
    PyObjGuard model_guard{response_model_local};

    const auto& spec = *spec_ptr;

    // ── OPT: Skip kwargs for zero-param endpoints (e.g. GET /) ──────────
    bool needs_kwargs = (match->param_count > 0) ||
        spec.has_query_params || spec.has_header_params ||
        spec.has_cookie_params || has_body_params_local ||
        spec.has_dependencies || !req.query_string.empty();

    PyRef kwargs(needs_kwargs ? PyDict_New() : nullptr);
    if (needs_kwargs && !kwargs) return nullptr;

    // ── Path parameters — O(1) hash map lookup ────────────────────────
    if (needs_kwargs && match->param_count > 0) {
        for (int pi = 0; pi < match->param_count; pi++) {
            auto pname = match->params[pi].name;
            auto pval = match->params[pi].value;
            auto pit = spec.path_map.find(pname);
            if (pit != spec.path_map.end()) {
                const auto& fs = spec.path_specs[pit->second];
                PyObject* py_val = coerce_param(pval, fs.type_tag);
                if (!py_val) return nullptr;
                PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val);
                Py_DECREF(py_val);
            } else {
                PyRef key(PyUnicode_FromStringAndSize(pname.data(), pname.size()));
                PyRef val(PyUnicode_FromStringAndSize(pval.data(), pval.size()));
                PyDict_SetItem(kwargs.get(), key.get(), val.get());
            }
        }
    }

    // ── Query parameters (O(1) hash map lookup) ────────────────────────
    if (spec.has_query_params && !req.query_string.empty()) {
        const char* p = req.query_string.data;
        const char* end = p + req.query_string.len;
        while (p < end) {
            const char* key_start = p;
            const char* eq = nullptr;
            while (p < end && *p != '&') {
                if (*p == '=' && !eq) eq = p;
                p++;
            }
            if (eq) {
                std::string_view key_sv(key_start, eq - key_start);
                std::string_view val_sv(eq + 1, p - eq - 1);

                auto qit = spec.query_map.find(key_sv);
                if (qit != spec.query_map.end()) {
                    const auto& fs = spec.query_specs[qit->second];
                    PyObject* py_val = coerce_param(val_sv, fs.type_tag);
                    if (!py_val) return nullptr;
                    PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val);
                    Py_DECREF(py_val);
                }
            }
            if (p < end) p++;  // skip '&'
        }
    }

    // ── Header + Cookie extraction (O(1) hash map lookup) ──────────────
    if (spec.has_header_params || spec.has_cookie_params) {
        for (int i = 0; i < req.header_count; i++) {
            const auto& hdr = req.headers[i];

            // Cookie header
            if (spec.has_cookie_params && hdr.name.iequals("cookie", 6)) {
                const char* cp = hdr.value.data;
                const char* cend = cp + hdr.value.len;
                while (cp < cend) {
                    while (cp < cend && (*cp == ' ' || *cp == ';')) cp++;
                    const char* ck_start = cp;
                    while (cp < cend && *cp != '=') cp++;
                    if (cp >= cend) break;
                    std::string_view cookie_name(ck_start, cp - ck_start);
                    cp++;  // skip '='
                    const char* cv_start = cp;
                    while (cp < cend && *cp != ';') cp++;
                    std::string_view cookie_val(cv_start, cp - cv_start);

                    auto cit = spec.cookie_map.find(cookie_name);
                    if (cit != spec.cookie_map.end()) {
                        const auto& fs = spec.cookie_specs[cit->second];
                        PyRef py_val(PyUnicode_FromStringAndSize(cookie_val.data(), cookie_val.size()));
                        PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                    }
                }
                continue;
            }

            // Header params
            if (spec.has_header_params) {
                char norm_buf[256];
                size_t norm_len = hdr.name.len < 255 ? hdr.name.len : 255;
                for (size_t j = 0; j < norm_len; j++) {
                    char c = hdr.name.data[j];
                    norm_buf[j] = (c >= 'A' && c <= 'Z') ? c + 32 : (c == '-') ? '_' : c;
                }
                std::string_view normalized(norm_buf, norm_len);
                auto hit = spec.header_map.find(normalized);
                if (hit != spec.header_map.end()) {
                    const auto& fs = spec.header_specs[hit->second];
                    PyRef py_val(PyUnicode_FromStringAndSize(hdr.value.data, hdr.value.len));
                    PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                }
            }
        }
    }

    // ── Fill defaults for missing params ─────────────────────────────────
    auto fill_defaults_http = [&](const std::vector<FieldSpec>& specs) {
        for (const auto& fs : specs) {
            if (fs.default_value && !PyDict_Contains(kwargs.get(), fs.py_field_name)) {
                PyDict_SetItem(kwargs.get(), fs.py_field_name, fs.default_value);
            }
        }
    };
    fill_defaults_http(spec.query_specs);
    fill_defaults_http(spec.header_specs);
    fill_defaults_http(spec.cookie_specs);

    // ── Body parsing: JSON or Form data ────────────────────────────────────
    PyObject* json_body_obj = Py_None;
    if (!req.body.empty()) {
        if (is_form_local && content_type_sv.len > 0) {
            // ── Form data parsing (urlencoded or multipart) ──────────────
            // Check content-type to determine form type
            bool is_urlencoded = false, is_multipart = false;
            std::string ct_check(content_type_sv.data, content_type_sv.len);
            for (auto& c : ct_check) if (c >= 'A' && c <= 'Z') c += 32;

            if (ct_check.find("application/x-www-form-urlencoded") != std::string::npos) {
                is_urlencoded = true;
            } else if (ct_check.find("multipart/form-data") != std::string::npos) {
                is_multipart = true;
            }

            if (is_urlencoded) {
                // Parse urlencoded body → list of (key, value) tuples → merge into kwargs
                const char* p = req.body.data;
                const char* end = p + req.body.len;
                PyRef form_dict(PyDict_New());
                while (p < end) {
                    const char* key_start = p;
                    const char* eq = nullptr;
                    while (p < end && *p != '&') {
                        if (*p == '=' && !eq) eq = p;
                        p++;
                    }
                    if (eq) {
                        // Percent-decode inline (simple form values)
                        std::string_view key_sv(key_start, eq - key_start);
                        std::string_view val_sv(eq + 1, p - eq - 1);
                        PyRef pk(PyUnicode_FromStringAndSize(key_sv.data(), key_sv.size()));
                        PyRef pv(PyUnicode_FromStringAndSize(val_sv.data(), val_sv.size()));
                        if (pk && pv) {
                            PyDict_SetItem(kwargs.get(), pk.get(), pv.get());
                            PyDict_SetItem(form_dict.get(), pk.get(), pv.get());
                        }
                    }
                    if (p < end) p++;
                }
                json_body_obj = form_dict.release();  // for Pydantic validation path
            } else if (is_multipart) {
                // Extract boundary from content-type
                size_t bpos = ct_check.find("boundary=");
                if (bpos != std::string::npos) {
                    bpos += 9;  // skip "boundary="
                    size_t bend = ct_check.find(';', bpos);
                    if (bend == std::string::npos) bend = ct_check.size();
                    // Use original (not lowered) content-type for boundary
                    std::string boundary(content_type_sv.data + bpos, bend - bpos);
                    // Strip quotes if present
                    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
                        boundary = boundary.substr(1, boundary.size() - 2);
                    }

                    // Call C++ multipart parser via Python wrapper
                    PyRef body_bytes(PyBytes_FromStringAndSize(req.body.data, req.body.len));
                    PyRef boundary_str(PyUnicode_FromStringAndSize(boundary.c_str(), boundary.size()));
                    if (body_bytes && boundary_str) {
                        PyRef parse_args(PyTuple_Pack(2, body_bytes.get(), boundary_str.get()));
                        // py_parse_multipart_body is declared in form_parser
                        extern PyObject* py_parse_multipart_body(PyObject*, PyObject*);
                        PyRef parts(py_parse_multipart_body(nullptr, parse_args.get()));
                        if (parts && PyList_Check(parts.get())) {
                            // Convert parts list to dict: name → data (or UploadFile-like)
                            PyRef form_dict(PyDict_New());
                            Py_ssize_t nparts = PyList_GET_SIZE(parts.get());
                            for (Py_ssize_t pi = 0; pi < nparts; pi++) {
                                PyObject* part = PyList_GET_ITEM(parts.get(), pi);  // borrowed
                                // PyDict_GetItemString returns borrowed ref — no PyRef
                                PyObject* name_obj = PyDict_GetItemString(part, "name");
                                PyObject* data_obj = PyDict_GetItemString(part, "data");
                                PyObject* fn_obj = PyDict_GetItemString(part, "filename");
                                if (name_obj) {
                                    if (fn_obj && fn_obj != Py_None) {
                                        // File upload — keep entire part dict
                                        PyDict_SetItem(form_dict.get(), name_obj, part);
                                        PyDict_SetItem(kwargs.get(), name_obj, part);
                                    } else if (data_obj) {
                                        // Simple form field — decode bytes to string
                                        if (PyBytes_Check(data_obj)) {
                                            char* d; Py_ssize_t dlen;
                                            PyBytes_AsStringAndSize(data_obj, &d, &dlen);
                                            PyRef str_val(PyUnicode_FromStringAndSize(d, dlen));
                                            if (str_val) {
                                                PyDict_SetItem(form_dict.get(), name_obj, str_val.get());
                                                PyDict_SetItem(kwargs.get(), name_obj, str_val.get());
                                            }
                                        } else {
                                            PyDict_SetItem(form_dict.get(), name_obj, data_obj);
                                            PyDict_SetItem(kwargs.get(), name_obj, data_obj);
                                        }
                                    }
                                }
                            }
                            json_body_obj = form_dict.release();
                        } else {
                            PyErr_Clear();
                        }
                    }
                }
            }
        } else if (spec.has_body_params) {
            // ── JSON body parsing (yyjson — GIL-released raw parse) ──────
            yyjson_doc* doc = nullptr;
            Py_BEGIN_ALLOW_THREADS
            doc = yyjson_parse_raw(req.body.data, req.body.len);
            Py_END_ALLOW_THREADS
            if (doc) {
                PyRef parsed(yyjson_doc_to_pyobject(doc));
                if (parsed) {
                    json_body_obj = parsed.release();
                } else {
                    PyErr_Clear();
                }
            }

            if (json_body_obj != Py_None && spec.body_param_name) {
                if (spec.embed_body_fields) {
                    if (PyDict_Check(json_body_obj)) {
                        PyDict_Update(kwargs.get(), json_body_obj);
                    }
                } else {
                    PyRef bp_key(PyUnicode_FromString(spec.body_param_name->c_str()));
                    PyDict_SetItem(kwargs.get(), bp_key.get(), json_body_obj);
                }
            }
        }
    }

    // ── Dependency injection (resolve Depends() callables) ──────────────
    if (spec.has_dependencies && spec.dep_solver) {
        // Inject raw request headers into kwargs for security dependencies
        // that need a Request object (e.g., HTTPBearer, HTTPBasic).
        // Uses interned keys for O(1) dict lookup in Python.
        {
            PyRef headers_list(PyList_New(req.header_count));
            if (headers_list) {
                for (int i = 0; i < req.header_count; i++) {
                    const auto& hdr = req.headers[i];
                    PyRef nb(PyBytes_FromStringAndSize(hdr.name.data, (Py_ssize_t)hdr.name.len));
                    PyRef vb(PyBytes_FromStringAndSize(hdr.value.data, (Py_ssize_t)hdr.value.len));
                    if (nb && vb) {
                        PyRef pair(PyTuple_Pack(2, nb.get(), vb.get()));
                        if (pair) {
                            PyList_SET_ITEM(headers_list.get(), i, pair.release());
                        } else {
                            Py_INCREF(Py_None);
                            PyList_SET_ITEM(headers_list.get(), i, Py_None);
                        }
                    } else {
                        Py_INCREF(Py_None);
                        PyList_SET_ITEM(headers_list.get(), i, Py_None);
                    }
                }
                static PyObject* s_rh_key = nullptr;
                if (!s_rh_key) s_rh_key = PyUnicode_InternFromString("__raw_headers__");
                PyDict_SetItem(kwargs.get(), s_rh_key, headers_list.get());
            }

            static PyObject* s_m_key = nullptr;
            if (!s_m_key) s_m_key = PyUnicode_InternFromString("__method__");
            PyRef method_str(PyUnicode_FromStringAndSize(req.method.data, (Py_ssize_t)req.method.len));
            if (method_str) PyDict_SetItem(kwargs.get(), s_m_key, method_str.get());

            static PyObject* s_p_key = nullptr;
            if (!s_p_key) s_p_key = PyUnicode_InternFromString("__path__");
            PyRef path_str(PyUnicode_FromStringAndSize(req.path.data, (Py_ssize_t)req.path.len));
            if (path_str) PyDict_SetItem(kwargs.get(), s_p_key, path_str.get());

            // Inject parsed Authorization header (scheme + credentials)
            // Python dep solver uses these for native HTTPBearer/HTTPBasic handling
            if (!authorization_sv.empty()) {
                size_t space_pos = 0;
                while (space_pos < authorization_sv.len && authorization_sv.data[space_pos] != ' ')
                    space_pos++;

                static PyObject* s_as_key = nullptr;
                if (!s_as_key) s_as_key = PyUnicode_InternFromString("__auth_scheme__");
                static PyObject* s_ac_key = nullptr;
                if (!s_ac_key) s_ac_key = PyUnicode_InternFromString("__auth_credentials__");

                PyRef scheme(PyUnicode_FromStringAndSize(authorization_sv.data, (Py_ssize_t)space_pos));
                size_t cred_start = space_pos < authorization_sv.len ? space_pos + 1 : space_pos;
                PyRef creds(PyUnicode_FromStringAndSize(
                    authorization_sv.data + cred_start,
                    (Py_ssize_t)(authorization_sv.len - cred_start)));
                if (scheme) PyDict_SetItem(kwargs.get(), s_as_key, scheme.get());
                if (creds) PyDict_SetItem(kwargs.get(), s_ac_key, creds.get());
            }
        }

        // dep_solver may be sync (returns tuple) or async (returns coroutine).
        // Detect at runtime: if result is a coroutine, drive it; if tuple, use directly.
        PyRef dep_call_result(PyObject_CallOneArg(spec.dep_solver, kwargs.get()));
        if (dep_call_result) {
            PyObject* dep_raw = nullptr;
            bool dep_resolved = false;

            if (PyCoro_CheckExact(dep_call_result.get())) {
                // Async dep solver — drive coroutine
                PySendResult dep_send = PyIter_Send(dep_call_result.get(), Py_None, &dep_raw);

                if (dep_send == PYGEN_RETURN && dep_raw) {
                    dep_resolved = true;
                } else if (dep_send == PYGEN_NEXT) {
                    // Async dependency — return to Python for awaiting
                    // dep_raw is the yielded Future — pass it so Python can await it properly

                    // Reset _asyncio_future_blocking — C++ intercepted the yield
                    // before asyncio's Task.__step could clear it.
                    static PyObject* s_fut_blocking_di = nullptr;
                    if (!s_fut_blocking_di) s_fut_blocking_di = PyUnicode_InternFromString("_asyncio_future_blocking");
                    if (dep_raw) {
                        PyObject_SetAttr(dep_raw, s_fut_blocking_di, Py_False);
                    }

                    // NOTE: Do NOT decrement active_requests here — Python will call
                    // record_request_end() after async DI completion
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    Py_XDECREF(body_params_local);

                    static PyObject* s_async_di_tag = nullptr;
                    if (!s_async_di_tag) s_async_di_tag = PyUnicode_InternFromString("async_di");
                    Py_INCREF(s_async_di_tag);

                    PyObject* ka = req.keep_alive ? Py_True : Py_False;
                    Py_INCREF(ka);
                    PyRef di_info(PyTuple_Pack(7, s_async_di_tag, dep_call_result.release(),
                                 dep_raw, endpoint_local, kwargs.release(),
                                 get_cached_status(status_code_local), ka));
                    Py_XDECREF(dep_raw);
                    return make_consumed_obj(req.total_consumed, di_info.release());
                } else {
                    // Async dep_solver raised immediately (e.g., HTTPException from auth check)
                    if (PyErr_Occurred()) {
                        PyObject *dep_et, *dep_ev, *dep_tb;
                        PyErr_Fetch(&dep_et, &dep_ev, &dep_tb);
                        int hook_status = 500;
                        if (is_http_exception(dep_et)) {
                            PyErr_NormalizeException(&dep_et, &dep_ev, &dep_tb);
                            PyRef detail(PyObject_GetAttrString(dep_ev, "detail"));
                            PyRef sc(PyObject_GetAttrString(dep_ev, "status_code"));
                            int scode = sc ? (int)PyLong_AsLong(sc.get()) : 500;
                            if (scode == -1 && PyErr_Occurred()) { PyErr_Clear(); scode = 500; }
                            hook_status = scode;
                            PyRef detail_str(detail ? PyObject_Str(detail.get()) : nullptr);
                            const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                            Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                            PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error",
                                       req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                            if (resp) write_to_transport(transport, resp.get());
                        } else {
                            Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                            PyRef resp(build_http_error_response(500, "Internal Server Error",
                                       req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                            if (resp) write_to_transport(transport, resp.get());
                        }
                        fire_post_response_hook(self, req.method.data, req.method.len,
                                                req.path.data, req.path.len, hook_status, request_start_time);
                    }
                    self->active_requests.fetch_sub(1, std::memory_order_release);
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    Py_DECREF(endpoint_local);
                    Py_XDECREF(body_params_local);
                    return make_consumed_true(req.total_consumed);
                }
            } else if (PyTuple_Check(dep_call_result.get())) {
                // Sync dep solver — result is already the tuple, no coroutine overhead
                dep_raw = dep_call_result.get();
                Py_INCREF(dep_raw);  // keep alive while we inspect it
                dep_resolved = true;
            }

            if (dep_resolved && dep_raw) {
                if (PyTuple_Check(dep_raw) && PyTuple_GET_SIZE(dep_raw) >= 2) {
                    PyObject* dep_values = PyTuple_GET_ITEM(dep_raw, 0);
                    PyObject* dep_errors = PyTuple_GET_ITEM(dep_raw, 1);

                    // Merge resolved dependency values into kwargs
                    if (dep_values && PyDict_Check(dep_values)) {
                        PyDict_Update(kwargs.get(), dep_values);
                    }

                    // If there are validation errors, return 422
                    if (dep_errors && PyList_Check(dep_errors) && PyList_GET_SIZE(dep_errors) > 0) {
                        Py_DECREF(dep_raw);
                        self->active_requests.fetch_sub(1, std::memory_order_release);
                        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                        Py_DECREF(endpoint_local);
                        Py_XDECREF(body_params_local);
                        PyRef resp(build_http_error_response(422, "Validation Error", req.keep_alive,
                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                        if (resp) write_to_transport(transport, resp.get());
                        return make_consumed_true(req.total_consumed);
                    }
                }
                Py_DECREF(dep_raw);
            }
        } else {
            // Sync dep_solver raised — check for HTTPException (auth/permission errors)
            if (PyErr_Occurred()) {
                PyObject *dep_et, *dep_ev, *dep_tb;
                PyErr_Fetch(&dep_et, &dep_ev, &dep_tb);
                int hook_status = 500;
                if (is_http_exception(dep_et)) {
                    PyErr_NormalizeException(&dep_et, &dep_ev, &dep_tb);
                    PyRef detail(PyObject_GetAttrString(dep_ev, "detail"));
                    PyRef sc(PyObject_GetAttrString(dep_ev, "status_code"));
                    int scode = sc ? (int)PyLong_AsLong(sc.get()) : 500;
                    if (scode == -1 && PyErr_Occurred()) { PyErr_Clear(); scode = 500; }
                    hook_status = scode;
                    PyRef detail_str(detail ? PyObject_Str(detail.get()) : nullptr);
                    const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                    Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                    PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error",
                               req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                    if (resp) write_to_transport(transport, resp.get());
                } else {
                    Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                    PyRef resp(build_http_error_response(500, "Internal Server Error",
                               req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                    if (resp) write_to_transport(transport, resp.get());
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, hook_status, request_start_time);
            }
            self->active_requests.fetch_sub(1, std::memory_order_release);
            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
            Py_DECREF(endpoint_local);
            Py_XDECREF(body_params_local);
            return make_consumed_true(req.total_consumed);
        }
    }

    // ── Pydantic body validation — inline from C++ (no event loop round-trip) ──
    // request_body_to_args() is async def but NEVER actually awaits for JSON bodies
    // (only awaits for FormData). So PyIter_Send completes inline → PYGEN_RETURN.
    // This makes POST JSON follow the exact same zero-transition path as GET.
    if (has_body_params_local && body_params_local) {

        // ── FAST PATH: plain dict body (no Pydantic model) ──────────────
        // For routes like `body: dict = Body(...)`, skip validation entirely
        if (spec.body_is_plain_dict && json_body_obj != Py_None && spec.body_param_name) {
            PyRef bp_key(PyUnicode_FromString(spec.body_param_name->c_str()));
            PyDict_SetItem(kwargs.get(), bp_key.get(), json_body_obj);
            Py_XDECREF(body_params_local);
            goto body_done;
        }

        // ── FAST PATH: single Pydantic model → call model_validate directly ──
        // Avoids going through request_body_to_args async wrapper
        if (spec.model_validate && json_body_obj != Py_None && spec.body_param_name) {
            PyRef validated(PyObject_CallOneArg(spec.model_validate, json_body_obj));
            if (validated) {
                PyRef bp_key(PyUnicode_FromString(spec.body_param_name->c_str()));
                PyDict_SetItem(kwargs.get(), bp_key.get(), validated.get());
                Py_XDECREF(body_params_local);
                goto body_done;
            } else {
                // Validation error — build 422 from exception
                PyObject *exc_type, *exc_val, *exc_tb;
                PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
                if (exc_val) {
                    PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
                    // Try to get .errors() from Pydantic ValidationError
                    PyRef errors_method(PyObject_GetAttrString(exc_val, "errors"));
                    if (errors_method) {
                        PyRef error_list(PyObject_CallNoArgs(errors_method.get()));
                        if (error_list) {
                            PyRef err_dict(PyDict_New());
                            if (err_dict) {
                                if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                                PyDict_SetItem(err_dict.get(), s_detail_key_global, error_list.get());
                                PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                                if (err_json) {
                                    char* ej_data; Py_ssize_t ej_len;
                                    PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                    PyRef resp(build_http_response_bytes(422, ej_data, (size_t)ej_len, req.keep_alive,
                                               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                                    if (resp) write_to_transport(transport, resp.get());
                                }
                            }
                            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                            Py_DECREF(endpoint_local);
                            Py_DECREF(body_params_local);
                            self->active_requests.fetch_sub(1, std::memory_order_release);
                            self->total_errors.fetch_add(1, std::memory_order_relaxed);
                            return make_consumed_true(req.total_consumed);
                        }
                    }
                }
                Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                PyErr_Clear();
                // Fall through to standard validation path
            }
        }

        // Lazy-load request_body_to_args function (one-time)
        if (!s_request_body_to_args) {
            PyRef deps_mod(PyImport_ImportModule("fastapi.dependencies.utils"));
            if (deps_mod) {
                s_request_body_to_args = PyObject_GetAttrString(deps_mod.get(), "request_body_to_args");
            }
        }

        bool inline_ok = false;
        if (s_request_body_to_args) {
            // Build kwargs: body_fields, received_body, embed_body_fields
            PyRef call_kw(PyDict_New());
            if (call_kw) {
                // Pre-interned by init_cached_refs() — fallback if not called
                if (!s_kw_body_fields) {
                    s_kw_body_fields = PyUnicode_InternFromString("body_fields");
                    s_kw_received_body = PyUnicode_InternFromString("received_body");
                    s_kw_embed = PyUnicode_InternFromString("embed_body_fields");
                }

                PyDict_SetItem(call_kw.get(), s_kw_body_fields, body_params_local);
                PyDict_SetItem(call_kw.get(), s_kw_received_body, json_body_obj);
                PyObject* embed_val = embed_body_local ? Py_True : Py_False;
                PyDict_SetItem(call_kw.get(), s_kw_embed, embed_val);

                // Call request_body_to_args() → returns coroutine
                PyRef body_coro(PyObject_Call(s_request_body_to_args, g_empty_tuple, call_kw.get()));
                if (body_coro) {
                    // Drive coroutine — for JSON bodies, completes inline (PYGEN_RETURN)
                    PyObject* validation_result = nullptr;
                    PySendResult vr_status = PyIter_Send(body_coro.get(), Py_None, &validation_result);

                    if (vr_status == PYGEN_RETURN && validation_result) {
                        if (PyTuple_Check(validation_result) && PyTuple_GET_SIZE(validation_result) >= 2) {
                            PyObject* body_values = PyTuple_GET_ITEM(validation_result, 0);  // borrowed
                            PyObject* body_errors = PyTuple_GET_ITEM(validation_result, 1);  // borrowed

                            if (body_errors && PyList_Check(body_errors) && PyList_GET_SIZE(body_errors) > 0) {
                                // ── Validation error → 422 with detailed error list ──
                                // Build {"detail": errors_list} and serialize as JSON
                                PyRef err_dict(PyDict_New());
                                if (err_dict) {
                                    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                                    PyDict_SetItem(err_dict.get(), s_detail_key_global, body_errors);
                                    PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                                    if (err_json) {
                                        char* ej_data; Py_ssize_t ej_len;
                                        PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                        PyRef resp(build_http_response_bytes(422, ej_data, (size_t)ej_len, req.keep_alive,
                                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                                        if (resp) write_to_transport(transport, resp.get());
                                    }
                                }
                                Py_DECREF(validation_result);
                                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                                Py_DECREF(endpoint_local);
                                Py_DECREF(body_params_local);
                                self->active_requests.fetch_sub(1, std::memory_order_release);
                                self->total_errors.fetch_add(1, std::memory_order_relaxed);
                                return make_consumed_true(req.total_consumed);
                            }

                            // ── Validation succeeded — merge values into kwargs ──
                            if (body_values && PyDict_Check(body_values)) {
                                PyDict_Update(kwargs.get(), body_values);
                            }
                            Py_DECREF(validation_result);
                            inline_ok = true;
                        } else {
                            Py_XDECREF(validation_result);
                        }
                    } else if (vr_status == PYGEN_NEXT) {
                        // Coroutine yielded — actual async (FormData). Fall back.
                        PyErr_Clear();
                    } else {
                        // PYGEN_ERROR
                        PyErr_Clear();
                    }
                } else {
                    PyErr_Clear();
                }
            }
        }

        if (!inline_ok) {
            // ── Fallback: InlineResult for async body (FormData) or errors ──
            InlineResultObject* ir = PyObject_New(InlineResultObject, &InlineResultType);
            if (!ir) {
                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                Py_DECREF(endpoint_local);
                Py_DECREF(body_params_local);
                return nullptr;
            }

            ir->status_code_obj = get_cached_status(status_code_local);
            ir->has_body_params = Py_True; Py_INCREF(Py_True);
            ir->embed_body_fields = embed_body_local ? Py_True : Py_False;
            Py_INCREF(ir->embed_body_fields);
            ir->kwargs = kwargs.release();
            if (json_body_obj != Py_None) {
                ir->json_body = json_body_obj;
            } else {
                Py_INCREF(Py_None);
                ir->json_body = Py_None;
            }
            ir->endpoint = endpoint_local;
            ir->body_params = body_params_local;

            self->active_requests.fetch_sub(1, std::memory_order_release);
            return make_consumed_obj(req.total_consumed, (PyObject*)ir);
        }
    }

    // Clean up body_params_local if not used in InlineResult
    Py_XDECREF(body_params_local);

body_done:
    // ── CALL ENDPOINT FROM C++ (OPT-9: fast-call) ──────────────────────
    // Use PyObject_CallNoArgs for zero-param endpoints (fastest possible call).
    // Use PyObject_VectorcallDict for endpoints with params (avoids tuple creation).
    PyRef coro(nullptr);
    {
        Py_ssize_t nkw = kwargs ? PyDict_GET_SIZE(kwargs.get()) : 0;
        if (nkw == 0) {
            // Zero params — fastest path: no args tuple, no kwargs dict
            coro = PyRef(PyObject_CallNoArgs(endpoint_local));
        } else {
            // Use vectorcall dict — avoids creating empty args tuple
            coro = PyRef(PyObject_VectorcallDict(endpoint_local, nullptr, 0, kwargs.get()));
        }
    }
    Py_DECREF(endpoint_local);  // release our strong ref
    if (!coro) {
        if (PyErr_Occurred()) {
            // ── Exception handler dispatch ──────────────────────────────
            PyObject *exc_type, *exc_val, *exc_tb;
            PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
            if (is_http_exception(exc_type)) {
                PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
                PyRef detail(PyObject_GetAttrString(exc_val, "detail"));
                PyRef sc(PyObject_GetAttrString(exc_val, "status_code"));
                int scode = sc ? (int)PyLong_AsLong(sc.get()) : 500;
                if (scode == -1 && PyErr_Occurred()) { PyErr_Clear(); scode = 500; }

                // Check custom exception handlers first
                auto eh_it = self->exception_handlers.find((uint16_t)scode);
                if (eh_it != self->exception_handlers.end()) {
                    // Call handler(request_dict, exc) — handler returns Response
                    PyRef handler_result(PyObject_CallOneArg(eh_it->second, exc_val));
                    Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                    exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
                    if (handler_result) {
                        // Try to extract response body from handler result
                        PyRef body_attr(PyObject_GetAttrString(handler_result.get(), "body"));
                        if (body_attr && PyBytes_Check(body_attr.get())) {
                            char* hbody; Py_ssize_t hbody_len;
                            PyBytes_AsStringAndSize(body_attr.get(), &hbody, &hbody_len);
                            PyRef sc_attr(PyObject_GetAttrString(handler_result.get(), "status_code"));
                            int hsc = sc_attr ? (int)PyLong_AsLong(sc_attr.get()) : scode;
                            if (hsc == -1 && PyErr_Occurred()) { PyErr_Clear(); hsc = scode; }
                            PyRef resp(build_http_response_bytes(hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                            if (resp) write_to_transport(transport, resp.get());
                        }
                    } else {
                        // Handler raised an exception — clear it before falling through to 500
                        PyErr_Clear();
                    }
                    fire_post_response_hook(self, req.method.data, req.method.len,
                                            req.path.data, req.path.len, scode, request_start_time);
                    self->active_requests.fetch_sub(1, std::memory_order_release);
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    return make_consumed_true(req.total_consumed);
                }

                Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
                if (detail && sc) {
                    PyRef detail_str(PyObject_Str(detail.get()));
                    const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                    PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error", req.keep_alive,
                               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                    if (resp) write_to_transport(transport, resp.get());
                    fire_post_response_hook(self, req.method.data, req.method.len,
                                            req.path.data, req.path.len, scode, request_start_time);
                    self->active_requests.fetch_sub(1, std::memory_order_release);
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    return make_consumed_true(req.total_consumed);
                }
            }
            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
        }
        // 500 error
        PyRef resp(build_http_error_response(500, "Internal Server Error", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_to_transport(transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 500, request_start_time);
        self->active_requests.fetch_sub(1, std::memory_order_release);
        self->total_errors.fetch_add(1, std::memory_order_relaxed);
        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
        return make_consumed_true(req.total_consumed);
    }

    // Clean up json_body_obj if it was allocated
    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);

    // ── Sync vs async endpoint dispatch ──────────────────────────────────
    // For sync endpoints, PyObject_Call returns the result directly (no coroutine).
    // For async endpoints, it returns a coroutine we must drive via PyIter_Send.
    PyObject* raw_result = nullptr;
    PySendResult send_status;
    if (!is_coro_local) {
        // Sync endpoint — result is already the return value, no coroutine driving needed
        raw_result = coro.release();
        send_status = PYGEN_RETURN;
    } else {
        // Async endpoint — drive the coroutine
        send_status = PyIter_Send(coro.get(), Py_None, &raw_result);
    }

    if (send_status == PYGEN_RETURN) {
        // ── Response model validation ────────────────────────────────────
        // If route has a response_model, validate + serialize through Pydantic
        if (response_model_local && response_model_local != Py_None) {
            static PyObject* s_validate = nullptr;
            static PyObject* s_serialize = nullptr;
            if (!s_validate) s_validate = PyUnicode_InternFromString("validate_python");
            if (!s_serialize) s_serialize = PyUnicode_InternFromString("serialize_python");

            // field.validate_python(raw_result) → validated model
            PyRef validated(PyObject_CallMethodOneArg(response_model_local, s_validate, raw_result));
            if (!validated) {
                // Validation error → 422
                PyErr_Clear();
                Py_DECREF(raw_result);
                PyRef resp(build_http_error_response(422, "Response validation error", req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                if (resp) write_to_transport(transport, resp.get());
                self->active_requests.fetch_sub(1, std::memory_order_release);
                return make_consumed_true(req.total_consumed);
            }

            // field.serialize_python(validated) → serializable dict
            PyRef serialized(PyObject_CallMethodOneArg(response_model_local, s_serialize, validated.get()));
            if (serialized) {
                Py_DECREF(raw_result);
                raw_result = serialized.release();  // Replace raw_result with serialized form
            } else {
                PyErr_Clear();
                // Fall through with original raw_result
            }
        }

        // Endpoint completed immediately — serialize + build HTTP response + write
        if (PyDict_Check(raw_result) || PyList_Check(raw_result) ||
            PyUnicode_Check(raw_result) || PyLong_Check(raw_result) ||
            PyFloat_Check(raw_result) || PyBool_Check(raw_result) ||
            PyTuple_Check(raw_result) || raw_result == Py_None) {

            // Serialize to JSON bytes (buffer pool)
            PyRef json_bytes(serialize_to_json_pybytes(raw_result));
            Py_DECREF(raw_result);

            if (!json_bytes) {
                PyRef resp(build_http_error_response(500, "JSON serialization failed", req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                if (resp) write_to_transport(transport, resp.get());
                self->active_requests.fetch_sub(1, std::memory_order_release);
                self->total_errors.fetch_add(1, std::memory_order_relaxed);
                return make_consumed_true(req.total_consumed);
            }

            char* json_data;
            Py_ssize_t json_len;
            PyBytes_AsStringAndSize(json_bytes.get(), &json_data, &json_len);

            // ── Compress response if client accepts and body is large enough ──
            const char* encoding = nullptr;
            std::vector<char> compressed;
            if (accept_encoding_sv.len > 0 && json_len > 500) {
                compressed = acquire_buffer();
                encoding = try_compress_inline(
                    json_data, (size_t)json_len,
                    accept_encoding_sv.data, accept_encoding_sv.len,
                    compressed);
                if (encoding) {
                    json_data = compressed.data();
                    json_len = (Py_ssize_t)compressed.size();
                }
            }

            // Build complete HTTP response in one buffer (with CORS + compression headers)
            PyRef http_resp(build_http_response_bytes(
                status_code_local, json_data, (size_t)json_len, req.keep_alive,
                has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                encoding));

            if (!compressed.empty()) release_buffer(std::move(compressed));

            if (http_resp) {
                write_to_transport(transport, http_resp.get());
            }

            fire_post_response_hook(self, req.method.data, req.method.len,
                                    req.path.data, req.path.len, status_code_local, request_start_time);
            self->active_requests.fetch_sub(1, std::memory_order_release);
            return make_consumed_true(req.total_consumed);
        }

        // ── Response object detection ─────────────────────────────────────
        // Check if result is a Starlette Response (has .body, .status_code, .raw_headers)
        PyRef body_attr(PyObject_GetAttrString(raw_result, "body"));
        PyRef sc_attr(PyObject_GetAttrString(raw_result, "status_code"));
        PyErr_Clear();  // Clear any AttributeError

        if (body_attr && PyBytes_Check(body_attr.get()) && sc_attr && PyLong_Check(sc_attr.get())) {
            // This is a Response object — extract body + status + headers
            int resp_sc = (int)PyLong_AsLong(sc_attr.get());
            char* resp_body; Py_ssize_t resp_body_len;
            PyBytes_AsStringAndSize(body_attr.get(), &resp_body, &resp_body_len);

            // Extract raw_headers to build custom header block
            PyRef raw_hdrs(PyObject_GetAttrString(raw_result, "raw_headers"));
            auto buf = acquire_buffer();
            buf.reserve(256 + (size_t)resp_body_len);

            // Build HTTP response status line (use cache if available)
            if (resp_sc > 0 && resp_sc < 600 && s_status_lines[resp_sc].data) {
                const auto& sl = s_status_lines[resp_sc];
                buf.insert(buf.end(), sl.data, sl.data + sl.len - 2);  // exclude trailing \r\n
            } else {
                static const char prefix[] = "HTTP/1.1 ";
                buf.insert(buf.end(), prefix, prefix + sizeof(prefix) - 1);
                char sc_buf[8];
                int sn = fast_i64_to_buf(sc_buf, resp_sc);
                buf.insert(buf.end(), sc_buf, sc_buf + sn);
                buf.push_back(' ');
                const char* reason = status_reason(resp_sc);
                size_t rlen = strlen(reason);
                buf.insert(buf.end(), reason, reason + rlen);
            }

            // Emit response headers from raw_headers list
            if (raw_hdrs && PyList_Check(raw_hdrs.get())) {
                Py_ssize_t nhdr = PyList_GET_SIZE(raw_hdrs.get());
                for (Py_ssize_t hi = 0; hi < nhdr; hi++) {
                    PyObject* htuple = PyList_GET_ITEM(raw_hdrs.get(), hi);
                    if (PyTuple_Check(htuple) && PyTuple_GET_SIZE(htuple) >= 2) {
                        PyObject* hname = PyTuple_GET_ITEM(htuple, 0);
                        PyObject* hval = PyTuple_GET_ITEM(htuple, 1);
                        if (PyBytes_Check(hname) && PyBytes_Check(hval)) {
                            buf.push_back('\r'); buf.push_back('\n');
                            buf.insert(buf.end(), PyBytes_AS_STRING(hname),
                                       PyBytes_AS_STRING(hname) + PyBytes_GET_SIZE(hname));
                            buf.push_back(':'); buf.push_back(' ');
                            buf.insert(buf.end(), PyBytes_AS_STRING(hval),
                                       PyBytes_AS_STRING(hval) + PyBytes_GET_SIZE(hval));
                        }
                    }
                }
            }

            // CORS headers
            if (has_cors && !origin_sv.empty()) {
                build_cors_headers(buf, cors_ptr, origin_sv.data, origin_sv.len);
            }

            // Connection + end of headers
            static const char CONN_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
            static const char CONN_CLOSE[] = "\r\nconnection: close\r\n\r\n";
            if (req.keep_alive) {
                buf.insert(buf.end(), CONN_KA, CONN_KA + sizeof(CONN_KA) - 1);
            } else {
                buf.insert(buf.end(), CONN_CLOSE, CONN_CLOSE + sizeof(CONN_CLOSE) - 1);
            }

            // Body
            buf.insert(buf.end(), resp_body, resp_body + resp_body_len);

            PyRef http_resp(PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size()));
            release_buffer(std::move(buf));
            if (http_resp) write_to_transport(transport, http_resp.get());

            // ── Background tasks ─────────────────────────────────────────
            PyRef bg_attr(PyObject_GetAttrString(raw_result, "background"));
            PyErr_Clear();
            if (bg_attr && bg_attr.get() != Py_None) {
                // Call background() to get coroutine, schedule on event loop
                PyRef bg_coro(PyObject_CallNoArgs(bg_attr.get()));
                if (bg_coro) {
                    // Schedule via asyncio.ensure_future (pre-imported by init_cached_refs)
                    if (!s_ensure_future) {
                        PyRef asyncio_mod(PyImport_ImportModule("asyncio"));
                        if (asyncio_mod) s_ensure_future = PyObject_GetAttrString(asyncio_mod.get(), "ensure_future");
                    }
                    if (s_ensure_future) {
                        PyRef fut(PyObject_CallOneArg(s_ensure_future, bg_coro.get()));
                        // fire-and-forget — fut can be discarded
                    }
                }
                PyErr_Clear();
            }

            fire_post_response_hook(self, req.method.data, req.method.len,
                                    req.path.data, req.path.len, resp_sc, request_start_time);
            Py_DECREF(raw_result);
            self->active_requests.fetch_sub(1, std::memory_order_release);
            return make_consumed_true(req.total_consumed);
        }

        // ── Fallback: serialize as string ────────────────────────────────
        PyRef str_repr(PyObject_Str(raw_result));
        Py_DECREF(raw_result);
        if (str_repr) {
            Py_ssize_t slen;
            const char* s = PyUnicode_AsUTF8AndSize(str_repr.get(), &slen);
            if (s) {
                PyRef resp(build_http_response_bytes(status_code_local, s, (size_t)slen, req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                if (resp) write_to_transport(transport, resp.get());
            }
        }
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, status_code_local, request_start_time);
        self->active_requests.fetch_sub(1, std::memory_order_release);
        return make_consumed_true(req.total_consumed);
    }

    if (send_status == PYGEN_NEXT) {
        // Endpoint suspended — needs real async I/O
        // Return (consumed, ("async", coro, yielded, status_code, keep_alive)) for Python
        // raw_result is the yielded Future — pass it so Python can await it properly
        // NOTE: Do NOT decrement active_requests here — Python will call
        // record_request_end() after async completion

        // Reset _asyncio_future_blocking — C++ intercepted the yield before
        // asyncio's Task.__step could clear it. Python needs to re-await this
        // future, which requires fut_blocking to be False.
        static PyObject* s_fut_blocking = nullptr;
        if (!s_fut_blocking) s_fut_blocking = PyUnicode_InternFromString("_asyncio_future_blocking");
        if (raw_result) {
            PyObject_SetAttr(raw_result, s_fut_blocking, Py_False);
        }

        static PyObject* s_async_tag = nullptr;
        if (!s_async_tag) s_async_tag = PyUnicode_InternFromString("async");
        Py_INCREF(s_async_tag);

        PyObject* ka = req.keep_alive ? Py_True : Py_False;
        Py_INCREF(ka);
        PyRef async_info(PyTuple_Pack(5, s_async_tag, coro.release(),
                         raw_result, get_cached_status(status_code_local), ka));
        Py_XDECREF(raw_result);
        return make_consumed_obj(req.total_consumed, async_info.release());
    }

    // PYGEN_ERROR — exception handler dispatch
    if (PyErr_Occurred()) {
        PyObject *exc_type, *exc_val, *exc_tb;
        PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
        if (is_http_exception(exc_type)) {
            PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
            PyRef detail(PyObject_GetAttrString(exc_val, "detail"));
            PyRef sc(PyObject_GetAttrString(exc_val, "status_code"));
            int scode = sc ? (int)PyLong_AsLong(sc.get()) : 500;

            // Check custom exception handlers
            auto eh_it = self->exception_handlers.find((uint16_t)scode);
            if (eh_it != self->exception_handlers.end()) {
                PyRef handler_result(PyObject_CallOneArg(eh_it->second, exc_val));
                Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
                if (handler_result) {
                    PyRef body_attr(PyObject_GetAttrString(handler_result.get(), "body"));
                    if (body_attr && PyBytes_Check(body_attr.get())) {
                        char* hbody; Py_ssize_t hbody_len;
                        PyBytes_AsStringAndSize(body_attr.get(), &hbody, &hbody_len);
                        PyRef sc_attr(PyObject_GetAttrString(handler_result.get(), "status_code"));
                        int hsc = sc_attr ? (int)PyLong_AsLong(sc_attr.get()) : scode;
                        PyRef resp(build_http_response_bytes(hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                        if (resp) write_to_transport(transport, resp.get());
                    }
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, scode, request_start_time);
                self->active_requests.fetch_sub(1, std::memory_order_release);
                return make_consumed_true(req.total_consumed);
            }

            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
            exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
            if (detail && sc) {
                PyRef detail_str(PyObject_Str(detail.get()));
                const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error", req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                if (resp) write_to_transport(transport, resp.get());
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, scode, request_start_time);
                self->active_requests.fetch_sub(1, std::memory_order_release);
                return make_consumed_true(req.total_consumed);
            }
        }
        Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
    }

    PyRef resp(build_http_error_response(500, "Internal Server Error", req.keep_alive,
               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
    if (resp) write_to_transport(transport, resp.get());
    fire_post_response_hook(self, req.method.data, req.method.len,
                            req.path.data, req.path.len, 500, request_start_time);
    self->active_requests.fetch_sub(1, std::memory_order_release);
    self->total_errors.fetch_add(1, std::memory_order_relaxed);
    return make_consumed_true(req.total_consumed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// serialize_and_write_http — for async endpoint results (METH_FASTCALL)
//
// Called from Python after awaiting an async endpoint's coroutine.
// args[0] = result (Python object to serialize)
// args[1] = transport (asyncio Transport)
// args[2] = status_code (int)
// args[3] = keep_alive (bool)
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_serialize_and_write_http(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs != 4) {
        PyErr_SetString(PyExc_TypeError, "serialize_and_write_http requires 4 args");
        return nullptr;
    }

    PyObject* content = args[0];
    PyObject* transport = args[1];
    int status_code = (int)PyLong_AsLong(args[2]);
    bool keep_alive = PyObject_IsTrue(args[3]);

    if (status_code == -1 && PyErr_Occurred()) return nullptr;

    // Serialize to JSON
    PyRef json_bytes(serialize_to_json_pybytes(content));
    if (!json_bytes) {
        PyRef resp(build_http_error_response(500, "JSON serialization failed", keep_alive));
        if (resp) write_to_transport(transport, resp.get());
        Py_RETURN_NONE;
    }

    char* json_data;
    Py_ssize_t json_len;
    PyBytes_AsStringAndSize(json_bytes.get(), &json_data, &json_len);

    PyRef http_resp(build_http_response_bytes(status_code, json_data, (size_t)json_len, keep_alive));
    if (http_resp) {
        write_to_transport(transport, http_resp.get());
    }

    Py_RETURN_NONE;
}

// ═══════════════════════════════════════════════════════════════════════════════
// build_response — returns complete HTTP response as PyBytes (METH_FASTCALL)
//
// Combines JSON serialization + HTTP response building in ONE call.
// Returns a single PyBytes containing the full HTTP response (headers + body).
// No intermediate PyBytes, no transport calls — Python writes it directly.
//
// args[0] = content (Python object to serialize as JSON)
// args[1] = status_code (int)
// args[2] = keep_alive (bool)
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_build_response(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs != 3) {
        PyErr_SetString(PyExc_TypeError, "build_response requires 3 args (content, status_code, keep_alive)");
        return nullptr;
    }

    PyObject* content = args[0];
    int status_code = (int)PyLong_AsLong(args[1]);
    if (status_code == -1 && PyErr_Occurred()) return nullptr;
    bool keep_alive = PyObject_IsTrue(args[2]);

    // Phase 1: Serialize JSON into buffer pool (one buffer only)
    auto json_buf = acquire_buffer();
    if (write_json(content, json_buf, 0) < 0) {
        release_buffer(std::move(json_buf));
        log_and_clear_pyerr("JSON serialization failed");
        return build_http_error_response(500, "JSON serialization failed", keep_alive);
    }

    size_t body_len = json_buf.size();

    // Phase 2: Compute header size, allocate PyBytes, write directly into it.
    // This eliminates the second buffer pool round-trip entirely.
    static const char HDR_200[] = "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: ";
    static const char CONN_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
    static const char CONN_CLOSE[] = "\r\nconnection: close\r\n\r\n";

    char cl_str[20];
    int cl_digits = fast_i64_to_buf(cl_str, (long long)body_len);

    const char* conn = keep_alive ? CONN_KA : CONN_CLOSE;
    size_t conn_len = keep_alive ? sizeof(CONN_KA) - 1 : sizeof(CONN_CLOSE) - 1;

    if (status_code == 200) {
        // Fast path: 200 OK — write directly into PyBytes (zero intermediate copies)
        size_t hdr_len = sizeof(HDR_200) - 1;
        size_t total = hdr_len + (size_t)cl_digits + conn_len + body_len;

        PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
        if (!result) { release_buffer(std::move(json_buf)); return nullptr; }

        char* dest = PyBytes_AS_STRING(result);
        memcpy(dest, HDR_200, hdr_len); dest += hdr_len;
        memcpy(dest, cl_str, (size_t)cl_digits); dest += cl_digits;
        memcpy(dest, conn, conn_len); dest += conn_len;
        memcpy(dest, json_buf.data(), body_len);

        release_buffer(std::move(json_buf));
        return result;
    }

    // General path: non-200 status codes — use build_http_response_bytes
    PyObject* result = build_http_response_bytes(
        status_code, json_buf.data(), json_buf.size(), keep_alive);
    release_buffer(std::move(json_buf));
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// PreparedRequest — returned by parse_and_route() for async dispatch
// All PyObject* fields, T_OBJECT_EX for zero-overhead attribute access.
// ═══════════════════════════════════════════════════════════════════════════════

static void PreparedRequest_dealloc(PreparedRequestObject* self) {
    Py_XDECREF(self->kwargs);
    Py_XDECREF(self->endpoint);
    Py_XDECREF(self->status_code_obj);
    Py_XDECREF(self->keep_alive_obj);
    Py_XDECREF(self->error_response);
    Py_XDECREF(self->has_body_params);
    Py_XDECREF(self->body_params);
    Py_XDECREF(self->embed_body_fields);
    Py_XDECREF(self->json_body);
    Py_XDECREF(self->is_coroutine);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyMemberDef PreparedRequest_members[] = {
    {"kwargs", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, kwargs), Py_READONLY, nullptr},
    {"endpoint", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, endpoint), Py_READONLY, nullptr},
    {"status_code", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, status_code_obj), Py_READONLY, nullptr},
    {"keep_alive", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, keep_alive_obj), Py_READONLY, nullptr},
    {"error_response", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, error_response), Py_READONLY, nullptr},
    {"has_body_params", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, has_body_params), Py_READONLY, nullptr},
    {"body_params", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, body_params), Py_READONLY, nullptr},
    {"embed_body_fields", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, embed_body_fields), Py_READONLY, nullptr},
    {"json_body", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, json_body), Py_READONLY, nullptr},
    {"is_coroutine", Py_T_OBJECT_EX, offsetof(PreparedRequestObject, is_coroutine), Py_READONLY, nullptr},
    {nullptr}
};

PyTypeObject PreparedRequestType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.PreparedRequest",
    .tp_basicsize = sizeof(PreparedRequestObject),
    .tp_dealloc = (destructor)PreparedRequest_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_members = PreparedRequest_members,
};

// ═══════════════════════════════════════════════════════════════════════════════
// parse_and_route — NON-BLOCKING parse + route + param extraction
//
// Does ONLY fast work (~15μs): HTTP parsing, route matching, parameter
// extraction, kwargs building. Does NOT call the endpoint, drive coroutines,
// serialize responses, or write to transport. Returns PreparedRequest for
// Python to dispatch asynchronously via create_task().
//
// args[0] = buffer (bytes/bytearray)
//
// Returns:
//   (0, None)                     — need more data
//   (-1, None)                    — parse error
//   (consumed, PreparedRequest)   — success, dispatch endpoint async
//   (consumed, InlineResult)      — needs Pydantic validation
//   (consumed, error_bytes)       — pre-built error response (404/405/500)
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_parse_and_route(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs != 1) {
        PyErr_SetString(PyExc_TypeError, "parse_and_route requires 1 arg (buffer)");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];

    Py_buffer view;
    if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) {
        return nullptr;
    }
    struct BufferGuard {
        Py_buffer* v;
        ~BufferGuard() { PyBuffer_Release(v); }
    } buf_guard{&view};

    char* buf_data = (char*)view.buf;
    Py_ssize_t buf_len = view.len;

    // ── Parse HTTP request ───────────────────────────────────────────────
    ParsedHttpRequest req = {};
    int parse_result = parse_http_request(buf_data, (size_t)buf_len, &req);

    if (parse_result == 0) {
        PyRef zero(PyLong_FromLong(0));
        Py_INCREF(Py_None);
        return PyTuple_Pack(2, zero.get(), Py_None);
    }

    if (parse_result < 0) {
        // Return pre-built 400 error bytes for Python to write directly
        PyRef err_resp(build_http_error_response(400, "Bad Request", false));
        if (!err_resp) {
            PyRef neg(PyLong_FromLong(-1));
            Py_INCREF(Py_None);
            return PyTuple_Pack(2, neg.get(), Py_None);
        }
        PyRef neg(PyLong_FromLong(-1));
        return PyTuple_Pack(2, neg.get(), err_resp.release());
    }

    // ── Route matching (skip lock if routes are frozen after startup) ────
    std::shared_lock<std::shared_mutex> lock(self->routes_mutex, std::defer_lock);
    if (!self->routes_frozen.load(std::memory_order_acquire)) {
        lock.lock();
    }
    auto match = self->router.at(req.path.data, req.path.len);
    if (!match) {
        if (lock.owns_lock()) lock.unlock();
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive));
        if (resp) return make_consumed_obj(req.total_consumed, resp.release());
        return make_consumed_true(req.total_consumed);
    }

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) {
        if (lock.owns_lock()) lock.unlock();
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive));
        if (resp) return make_consumed_obj(req.total_consumed, resp.release());
        return make_consumed_true(req.total_consumed);
    }

    const auto& route = self->routes[idx];

    // ── Method check (O(1) bitmask) ────────────────────────────────────
    if (route.method_mask) {
        uint8_t req_method = method_str_to_bit(req.method.data, req.method.len);
        if (!(route.method_mask & req_method)) {
            if (lock.owns_lock()) lock.unlock();
            PyRef resp(build_http_error_response(405, "Method Not Allowed", req.keep_alive));
            if (resp) return make_consumed_obj(req.total_consumed, resp.release());
            return make_consumed_true(req.total_consumed);
        }
    }

    if (!route.fast_spec) {
        if (lock.owns_lock()) lock.unlock();
        PyRef resp(build_http_error_response(500, "Route not configured", req.keep_alive));
        if (resp) return make_consumed_obj(req.total_consumed, resp.release());
        return make_consumed_true(req.total_consumed);
    }

    // Copy route data to locals and release lock early.
    const FastRouteSpec* spec_ptr = &(*route.fast_spec);
    PyObject* endpoint_local = route.endpoint;
    Py_INCREF(endpoint_local);
    uint16_t status_code_local = route.status_code;
    bool is_coroutine_local = route.is_coroutine;
    bool has_body_params_local = spec_ptr->has_body_params;
    PyObject* body_params_local = spec_ptr->body_params;
    if (body_params_local) Py_INCREF(body_params_local);
    bool embed_body_local = spec_ptr->embed_body_fields;

    if (lock.owns_lock()) lock.unlock();

    const auto& spec = *spec_ptr;

    // ── Build kwargs dict ────────────────────────────────────────────────
    PyRef kwargs(PyDict_New());
    if (!kwargs) { Py_DECREF(endpoint_local); Py_XDECREF(body_params_local); return nullptr; }

    // ── Path parameters ──────────────────────────────────────────────────
    if (match->param_count > 0) {
        for (int pi = 0; pi < match->param_count; pi++) {
            auto pname = match->params[pi].name;
            auto pval = match->params[pi].value;
            bool coerced = false;
            for (const auto& fs : spec.path_specs) {
                if (fs.field_name == pname) {
                    PyObject* py_val = coerce_param(pval, fs.type_tag);
                    if (!py_val) { Py_DECREF(endpoint_local); Py_XDECREF(body_params_local); return nullptr; }
                    if (PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val) < 0) { Py_DECREF(py_val); Py_DECREF(endpoint_local); Py_XDECREF(body_params_local); return nullptr; }
                    Py_DECREF(py_val);
                    coerced = true;
                    break;
                }
            }
            if (!coerced) {
                PyRef key(PyUnicode_FromStringAndSize(pname.data(), pname.size()));
                PyRef val(PyUnicode_FromStringAndSize(pval.data(), pval.size()));
                if (key && val) PyDict_SetItem(kwargs.get(), key.get(), val.get());
            }
        }
    }

    // ── Query parameters (O(1) hash map lookup) ────────────────────────
    if (spec.has_query_params && !req.query_string.empty()) {
        const char* p = req.query_string.data;
        const char* end = p + req.query_string.len;
        while (p < end) {
            const char* key_start = p;
            const char* eq = nullptr;
            while (p < end && *p != '&') {
                if (*p == '=' && !eq) eq = p;
                p++;
            }
            if (eq) {
                std::string_view key_sv(key_start, eq - key_start);
                std::string_view val_sv(eq + 1, p - eq - 1);

                auto qit = spec.query_map.find(key_sv);
                if (qit != spec.query_map.end()) {
                    const auto& fs = spec.query_specs[qit->second];
                    PyObject* py_val = coerce_param(val_sv, fs.type_tag);
                    if (py_val) {
                        PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val);
                        Py_DECREF(py_val);
                    }
                }
            }
            if (p < end) p++;
        }
    }

    // ── Header + Cookie extraction (O(1) hash map lookup) ──────────────
    if (spec.has_header_params || spec.has_cookie_params) {
        for (int i = 0; i < req.header_count; i++) {
            const auto& hdr = req.headers[i];

            if (spec.has_cookie_params && hdr.name.iequals("cookie", 6)) {
                const char* cp = hdr.value.data;
                const char* cend = cp + hdr.value.len;
                while (cp < cend) {
                    while (cp < cend && (*cp == ' ' || *cp == ';')) cp++;
                    const char* ck_start = cp;
                    while (cp < cend && *cp != '=') cp++;
                    if (cp >= cend) break;
                    std::string_view cookie_name(ck_start, cp - ck_start);
                    cp++;
                    const char* cv_start = cp;
                    while (cp < cend && *cp != ';') cp++;
                    std::string_view cookie_val(cv_start, cp - cv_start);

                    auto cit = spec.cookie_map.find(cookie_name);
                    if (cit != spec.cookie_map.end()) {
                        const auto& fs = spec.cookie_specs[cit->second];
                        PyRef py_val(PyUnicode_FromStringAndSize(cookie_val.data(), cookie_val.size()));
                        if (py_val) PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                    }
                }
                continue;
            }

            if (spec.has_header_params) {
                char norm_buf[256];
                size_t norm_len = hdr.name.len < 255 ? hdr.name.len : 255;
                for (size_t j = 0; j < norm_len; j++) {
                    char c = hdr.name.data[j];
                    norm_buf[j] = (c >= 'A' && c <= 'Z') ? c + 32 : (c == '-') ? '_' : c;
                }
                std::string_view normalized(norm_buf, norm_len);
                auto hit = spec.header_map.find(normalized);
                if (hit != spec.header_map.end()) {
                    const auto& fs = spec.header_specs[hit->second];
                    PyRef py_val(PyUnicode_FromStringAndSize(hdr.value.data, hdr.value.len));
                    if (py_val) PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                }
            }
        }
    }

    // ── Fill defaults for missing params ─────────────────────────────────
    auto fill_defaults = [&](const std::vector<FieldSpec>& specs) {
        for (const auto& fs : specs) {
            if (fs.default_value && !PyDict_Contains(kwargs.get(), fs.py_field_name)) {
                PyDict_SetItem(kwargs.get(), fs.py_field_name, fs.default_value);
            }
        }
    };
    fill_defaults(spec.query_specs);
    fill_defaults(spec.header_specs);
    fill_defaults(spec.cookie_specs);

    // ── JSON body parsing (yyjson — GIL-released raw parse) ──
    PyObject* json_body_obj = Py_None;
    if (spec.has_body_params && !req.body.empty()) {
        yyjson_doc* doc = nullptr;
        Py_BEGIN_ALLOW_THREADS
        doc = yyjson_parse_raw(req.body.data, req.body.len);
        Py_END_ALLOW_THREADS
        if (doc) {
            PyRef parsed(yyjson_doc_to_pyobject(doc));
            if (parsed) {
                json_body_obj = parsed.release();
            } else {
                PyErr_Clear();
            }
        }

        if (json_body_obj != Py_None && spec.body_param_name) {
            if (spec.embed_body_fields) {
                if (PyDict_Check(json_body_obj)) {
                    PyDict_Update(kwargs.get(), json_body_obj);
                }
            } else {
                PyRef bp_key(PyUnicode_FromString(spec.body_param_name->c_str()));
                if (bp_key) PyDict_SetItem(kwargs.get(), bp_key.get(), json_body_obj);
            }
        }
    }

    // ── If route has Pydantic body params — return InlineResult ──────────
    if (has_body_params_local && body_params_local) {
        InlineResultObject* ir = PyObject_New(InlineResultObject, &InlineResultType);
        if (!ir) {
            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
            Py_DECREF(endpoint_local);
            Py_DECREF(body_params_local);
            return nullptr;
        }

        ir->status_code_obj = get_cached_status(status_code_local);
        ir->has_body_params = Py_True; Py_INCREF(Py_True);
        ir->embed_body_fields = embed_body_local ? Py_True : Py_False;
        Py_INCREF(ir->embed_body_fields);
        ir->kwargs = kwargs.release();
        if (json_body_obj != Py_None) {
            ir->json_body = json_body_obj;
        } else {
            Py_INCREF(Py_None);
            ir->json_body = Py_None;
        }
        ir->endpoint = endpoint_local;
        ir->body_params = body_params_local;

        return make_consumed_obj(req.total_consumed, (PyObject*)ir);
    }

    // Clean up body_params_local if not used
    Py_XDECREF(body_params_local);

    // ── Build PreparedRequest for async dispatch ─────────────────────────
    PreparedRequestObject* prep = PyObject_New(PreparedRequestObject, &PreparedRequestType);
    if (!prep) {
        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
        Py_DECREF(endpoint_local);
        return nullptr;
    }

    prep->kwargs = kwargs.release();
    prep->endpoint = endpoint_local;  // transfer strong ref
    prep->status_code_obj = get_cached_status(status_code_local);
    prep->keep_alive_obj = req.keep_alive ? Py_True : Py_False;
    Py_INCREF(prep->keep_alive_obj);
    Py_INCREF(Py_None);
    prep->error_response = Py_None;
    prep->has_body_params = Py_False; Py_INCREF(Py_False);
    Py_INCREF(Py_None);
    prep->body_params = Py_None;
    prep->embed_body_fields = Py_False; Py_INCREF(Py_False);
    if (json_body_obj != Py_None) {
        prep->json_body = json_body_obj;
    } else {
        Py_INCREF(Py_None);
        prep->json_body = Py_None;
    }
    prep->is_coroutine = is_coroutine_local ? Py_True : Py_False;
    Py_INCREF(prep->is_coroutine);

    return make_consumed_obj(req.total_consumed, (PyObject*)prep);
}

// ── configure_rate_limit(enabled, max_requests, window_seconds) ────────────
static PyObject* CoreApp_configure_rate_limit(CoreAppObject* self, PyObject* args) {
    int enabled, max_req, window_sec;
    if (!PyArg_ParseTuple(args, "pii", &enabled, &max_req, &window_sec)) return nullptr;
    self->rate_limit_enabled = (bool)enabled;
    self->rate_limit_max_requests = max_req;
    self->rate_limit_window_seconds = window_sec;
    Py_RETURN_NONE;
}

// ── set_client_ip(ip_string) ───────────────────────────────────────────────
static PyObject* CoreApp_set_client_ip(CoreAppObject* self, PyObject* arg) {
    const char* ip = PyUnicode_AsUTF8(arg);
    if (!ip) return nullptr;
    self->current_client_ip = ip;
    Py_RETURN_NONE;
}

// ── set_post_response_hook(callable_or_None) ───────────────────────────────
static PyObject* CoreApp_set_post_response_hook(CoreAppObject* self, PyObject* arg) {
    Py_XDECREF(self->post_response_hook);
    if (arg == Py_None) {
        self->post_response_hook = nullptr;
    } else {
        Py_INCREF(arg);
        self->post_response_hook = arg;
    }
    Py_RETURN_NONE;
}

// ── Method table ────────────────────────────────────────────────────────────

static PyMethodDef CoreApp_methods[] = {
    {"next_route_id", (PyCFunction)CoreApp_next_route_id, METH_NOARGS, nullptr},
    {"record_request_start", (PyCFunction)CoreApp_record_request_start, METH_NOARGS, nullptr},
    {"record_request_end", (PyCFunction)CoreApp_record_request_end, METH_NOARGS, nullptr},
    {"record_error", (PyCFunction)CoreApp_record_error, METH_NOARGS, nullptr},
    {"route_count", (PyCFunction)CoreApp_route_count, METH_NOARGS, nullptr},
    {"get_metrics", (PyCFunction)CoreApp_get_metrics, METH_NOARGS, nullptr},
    {"get_routes", (PyCFunction)CoreApp_get_routes, METH_NOARGS, nullptr},
    {"add_route", (PyCFunction)CoreApp_add_route, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"match_request", (PyCFunction)CoreApp_match_request, METH_VARARGS, nullptr},
    {"get_endpoint", (PyCFunction)CoreApp_get_endpoint, METH_O, nullptr},
    {"get_response_model_field", (PyCFunction)CoreApp_get_response_model_field, METH_O, nullptr},
    {"get_response_filters", (PyCFunction)CoreApp_get_response_filters, METH_O, nullptr},
    {"register_fast_spec", (PyCFunction)CoreApp_register_fast_spec, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"configure_cors", (PyCFunction)CoreApp_configure_cors, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"configure_trusted_hosts", (PyCFunction)CoreApp_configure_trusted_hosts, METH_O, nullptr},
    {"check_trusted_host", (PyCFunction)CoreApp_check_trusted_host, METH_O, nullptr},
    {"get_route_info", (PyCFunction)CoreApp_get_route_info, METH_O, nullptr},
    {"add_exception_handler", (PyCFunction)CoreApp_add_exception_handler, METH_VARARGS, nullptr},
    // C++ HTTP server path — bypass ASGI entirely
    {"handle_http", (PyCFunction)(void(*)(void))CoreApp_handle_http, METH_FASTCALL, nullptr},
    {"serialize_and_write_http", (PyCFunction)(void(*)(void))CoreApp_serialize_and_write_http, METH_FASTCALL, nullptr},
    // Returns complete HTTP response as single PyBytes (no transport calls)
    {"build_response", (PyCFunction)(void(*)(void))CoreApp_build_response, METH_FASTCALL, nullptr},
    // Non-blocking parse+route — returns PreparedRequest for async dispatch
    {"parse_and_route", (PyCFunction)(void(*)(void))CoreApp_parse_and_route, METH_FASTCALL, nullptr},
    {"freeze_routes", (PyCFunction)CoreApp_freeze_routes, METH_NOARGS, nullptr},
    {"set_openapi_schema", (PyCFunction)CoreApp_set_openapi_schema, METH_O, nullptr},
    // C++ native middleware support
    {"configure_rate_limit", (PyCFunction)CoreApp_configure_rate_limit, METH_VARARGS, nullptr},
    {"set_client_ip", (PyCFunction)CoreApp_set_client_ip, METH_O, nullptr},
    {"set_post_response_hook", (PyCFunction)CoreApp_set_post_response_hook, METH_O, nullptr},
    {nullptr}
};

PyTypeObject CoreAppType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.CoreApp",
    .tp_basicsize = sizeof(CoreAppObject),
    .tp_dealloc = (destructor)CoreApp_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = CoreApp_methods,
    .tp_new = CoreApp_new,
};

// ── Module registration ─────────────────────────────────────────────────────

// ═══════════════════════════════════════════════════════════════════════════════
// Standalone response helpers (called from Python _cpp_server.py)
// ═══════════════════════════════════════════════════════════════════════════════

// build_response_from_parts(status_code, headers_list, body, keep_alive) → bytes
// Replaces _write_response_obj() Python f-string + b"".join() with single C++ call
PyObject* py_build_response_from_parts(PyObject* /*self*/, PyObject* args) {
    int status_code;
    PyObject* headers_list;
    Py_buffer body;
    int keep_alive;

    if (!PyArg_ParseTuple(args, "iOy*p", &status_code, &headers_list, &body, &keep_alive)) {
        return nullptr;
    }

    auto buf = acquire_buffer();
    size_t body_len = (size_t)body.len;
    buf.reserve(256 + body_len);

    // Status line (use cache if available)
    if (status_code >= 0 && status_code < 600 && s_status_lines[status_code].data) {
        const auto& sl = s_status_lines[status_code];
        buf.insert(buf.end(), sl.data, sl.data + sl.len);
    } else {
        static const char prefix[] = "HTTP/1.1 ";
        buf.insert(buf.end(), prefix, prefix + sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf.insert(buf.end(), sc_buf, sc_buf + sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf.insert(buf.end(), reason, reason + rlen);
        buf.insert(buf.end(), {'\r', '\n'});
    }

    // Headers from list of (key, value) tuples
    bool has_content_length = false;
    Py_ssize_t n_hdrs = PyList_Size(headers_list);
    for (Py_ssize_t i = 0; i < n_hdrs; i++) {
        PyObject* item = PyList_GET_ITEM(headers_list, i);
        PyObject* key = PyTuple_GET_ITEM(item, 0);
        PyObject* val = PyTuple_GET_ITEM(item, 1);

        Py_ssize_t klen, vlen;
        const char* kstr = PyUnicode_AsUTF8AndSize(key, &klen);
        const char* vstr = PyUnicode_AsUTF8AndSize(val, &vlen);
        if (!kstr || !vstr) {
            PyBuffer_Release(&body);
            release_buffer(std::move(buf));
            return nullptr;
        }

        buf.insert(buf.end(), kstr, kstr + klen);
        buf.insert(buf.end(), {':', ' '});
        buf.insert(buf.end(), vstr, vstr + vlen);
        buf.insert(buf.end(), {'\r', '\n'});

        // Check if content-length was provided (case-insensitive first char + length match)
        if (klen == 14) {
            char c0 = kstr[0];
            if ((c0 == 'c' || c0 == 'C') && strncasecmp(kstr, "content-length", 14) == 0) {
                has_content_length = true;
            }
        }
    }

    // Add content-length if not provided by headers
    if (!has_content_length) {
        static const char cl_pre[] = "content-length: ";
        buf.insert(buf.end(), cl_pre, cl_pre + sizeof(cl_pre) - 1);
        char cl_buf[20];
        int cl_n = fast_i64_to_buf(cl_buf, (long long)body_len);
        buf.insert(buf.end(), cl_buf, cl_buf + cl_n);
        buf.insert(buf.end(), {'\r', '\n'});
    }

    // Connection header
    if (keep_alive) {
        static const char ka[] = "connection: keep-alive\r\n";
        buf.insert(buf.end(), ka, ka + sizeof(ka) - 1);
    } else {
        static const char cl[] = "connection: close\r\n";
        buf.insert(buf.end(), cl, cl + sizeof(cl) - 1);
    }

    // End of headers + body
    buf.insert(buf.end(), {'\r', '\n'});
    buf.insert(buf.end(), (const char*)body.buf, (const char*)body.buf + body_len);
    PyBuffer_Release(&body);

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// build_chunked_frame(chunk: bytes) → bytes
// Builds "{hex_len}\r\n{chunk}\r\n" in a single allocation
// Replaces Python: f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n"
PyObject* py_build_chunked_frame(PyObject* /*self*/, PyObject* arg) {
    Py_buffer chunk;
    if (PyObject_GetBuffer(arg, &chunk, PyBUF_SIMPLE) < 0) {
        return nullptr;
    }

    size_t chunk_len = (size_t)chunk.len;
    if (chunk_len == 0) {
        PyBuffer_Release(&chunk);
        return PyBytes_FromStringAndSize("0\r\n\r\n", 5);
    }

    // Hex encode length: max 16 chars for size_t
    char hex_buf[20];
    int hex_len = snprintf(hex_buf, sizeof(hex_buf), "%zx", chunk_len);

    // Total: hex + \r\n + chunk + \r\n
    size_t total = (size_t)hex_len + 2 + chunk_len + 2;
    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
    if (!result) {
        PyBuffer_Release(&chunk);
        return nullptr;
    }

    char* out = PyBytes_AS_STRING(result);
    memcpy(out, hex_buf, (size_t)hex_len);
    out[hex_len] = '\r';
    out[hex_len + 1] = '\n';
    memcpy(out + hex_len + 2, chunk.buf, chunk_len);
    out[hex_len + 2 + chunk_len] = '\r';
    out[hex_len + 2 + chunk_len + 1] = '\n';

    PyBuffer_Release(&chunk);
    return result;
}

int register_app_types(PyObject* module) {
    if (PyType_Ready(&CoreAppType) < 0) return -1;
    if (PyType_Ready(&MatchResultType) < 0) return -1;
    if (PyType_Ready(&ResponseDataType) < 0) return -1;
    if (PyType_Ready(&InlineResultType) < 0) return -1;
    if (PyType_Ready(&PreparedRequestType) < 0) return -1;

    Py_INCREF(&CoreAppType);
    PyModule_AddObject(module, "CoreApp", (PyObject*)&CoreAppType);
    Py_INCREF(&MatchResultType);
    PyModule_AddObject(module, "MatchResult", (PyObject*)&MatchResultType);
    Py_INCREF(&ResponseDataType);
    PyModule_AddObject(module, "ResponseData", (PyObject*)&ResponseDataType);
    Py_INCREF(&InlineResultType);
    PyModule_AddObject(module, "InlineResult", (PyObject*)&InlineResultType);
    Py_INCREF(&PreparedRequestType);
    PyModule_AddObject(module, "PreparedRequest", (PyObject*)&PreparedRequestType);

    // Register cleanup for module-level cached refs
    Py_AtExit(cleanup_cached_refs);

    return 0;
}
