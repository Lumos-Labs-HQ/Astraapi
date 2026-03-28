#define PY_SSIZE_T_CLEAN
#include <Python.h>


#include "app.hpp"
#include "asgi_constants.hpp"
#include "json_writer.hpp"
#include "json_parser.hpp"
#include "buffer_pool.hpp"
#include "http_parser.hpp"
#include "ws_frame_parser.hpp"
#include "percent_decode.hpp"
#include "pyref.hpp"
#include <cstring>
#include "platform.hpp"
#include <mutex>
#include <new>
#include <algorithm>
#include <string_view>
#include <charconv>
#include <chrono>
#include <array>
#include <zlib.h>

// ── buf_append: resize+memcpy (5-15% faster than buf.insert iterator path) ──
static inline void buf_append(std::vector<char>& buf, const char* s, size_t len) {
    size_t old = buf.size();
    buf.resize(old + len);
    std::memcpy(buf.data() + old, s, len);
}
static inline void buf_append(std::vector<char>& buf, const std::string& s) {
    buf_append(buf, s.data(), s.size());
}

// ── Header normalization lookup table: lowercase + '-' → '_' in one table lookup ──
// Eliminates 3 branches per character in the header normalization loop.
static constexpr std::array<char, 256> make_header_norm_table() {
    std::array<char, 256> t{};
    for (int i = 0; i < 256; i++) t[i] = (char)i;
    for (int i = 'A'; i <= 'Z'; i++) t[i] = (char)(i + 32);
    t[(unsigned char)'-'] = '_';
    return t;
}
static constexpr auto s_header_norm = make_header_norm_table();

// ── Case-insensitive helpers (zero-allocation, for content-type/origin checks) ──
static inline bool ci_starts_with(const char* s, size_t s_len, const char* prefix, size_t p_len) {
    if (s_len < p_len) return false;
    for (size_t i = 0; i < p_len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != prefix[i]) return false;
    }
    return true;
}
static inline bool ci_contains(const char* s, size_t s_len, const char* needle, size_t n_len) {
    if (s_len < n_len) return false;
    for (size_t i = 0; i <= s_len - n_len; i++) {
        bool match = true;
        for (size_t j = 0; j < n_len; j++) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needle[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

#if HAS_BROTLI
#include <brotli/encode.h>
#endif
#ifndef HAS_BROTLI
#define HAS_BROTLI 0
#endif

// ── Module-level cached imports (consolidated, cleaned up at exit) ────────────
static PyObject* s_http_exc_type = nullptr;         // starlette.exceptions.HTTPException
static PyObject* s_astraapi_http_exc_type = nullptr;  // astraapi.exceptions.HTTPException
static PyObject* s_validation_exc_type = nullptr;    // astraapi.exceptions.RequestValidationError
static PyObject* s_resume_func = nullptr;            // astraapi._core_app._resume_coro
static PyObject* s_request_body_to_args = nullptr;   // astraapi.dependencies.utils.request_body_to_args
static PyObject* s_form_data_class = nullptr;         // astraapi._datastructures_impl.FormData
static PyObject* s_upload_file_class = nullptr;       // astraapi._datastructures_impl.UploadFile

// Pre-interned strings for transport method calls (cleaned up at exit)
static PyObject* g_str_write = nullptr;
static PyObject* g_str_is_closing = nullptr;

// Promoted from function-local statics for eager initialization
static PyObject* s_ensure_future = nullptr;          // asyncio.ensure_future
static PyObject* s_kw_body_fields = nullptr;         // "body_fields" interned string
static PyObject* s_kw_received_body = nullptr;       // "received_body" interned string
static PyObject* s_kw_embed = nullptr;               // "embed_body_fields" interned string
static PyObject* s_detail_key_global = nullptr;      // "detail" interned string
static PyObject* s_loc_key_global = nullptr;          // "loc" interned string
static PyObject* s_url_key_global = nullptr;          // "url" interned string
static PyObject* s_body_str_global = nullptr;         // "body" interned string
static PyObject* s_include_url_key = nullptr;         // "include_url" interned string
// Cached return tuples (avoid per-request PyObject allocation)
static PyObject* s_need_more_data = nullptr;         // (0, None) — returned on incomplete parse

// DI/async path interned strings (promoted from lazy function-local statics)
static PyObject* s_rh_key = nullptr;                 // "__raw_headers__"
static PyObject* s_m_key = nullptr;                  // "__method__"
static PyObject* s_p_key = nullptr;                  // "__path__"
static PyObject* s_as_key = nullptr;                 // "__auth_scheme__"
static PyObject* s_ac_key = nullptr;                 // "__auth_credentials__"
static PyObject* s_validate_str = nullptr;           // "validate_python"
static PyObject* s_fut_blocking = nullptr;           // "_asyncio_future_blocking"
static PyObject* s_async_tag = nullptr;              // "async"
static PyObject* s_stream_tag = nullptr;             // "stream"
// Pre-interned attribute name strings for Response object detection
static PyObject* s_attr_body_iterator = nullptr;     // "body_iterator"
static PyObject* s_attr_path = nullptr;              // "path"
static PyObject* s_attr_body = nullptr;              // "body"
static PyObject* s_attr_status_code = nullptr;       // "status_code"
static PyObject* s_attr_raw_headers = nullptr;       // "_raw_headers"
static PyObject* s_attr_raw_headers2 = nullptr;      // "raw_headers"
static PyObject* s_attr_background = nullptr;        // "background"
static PyObject* s_attr_headers = nullptr;           // "headers"

// Promoted from function-local statics (eliminates per-call lazy-init branch)
static PyObject* s_ct_type_key = nullptr;    // "type"
static PyObject* s_ct_loc_key = nullptr;     // "loc"
static PyObject* s_ct_msg_key = nullptr;     // "msg"
static PyObject* s_ct_input_key = nullptr;   // "input"
static PyObject* s_ct_mat_val = nullptr;     // "model_attributes_type"
static PyObject* s_ct_mat_msg = nullptr;     // "Input should be a valid..."
static PyObject* s_ct_body_str = nullptr;    // "body" (JSON error path)
static PyObject* s_body_key = nullptr;       // "__body__"
static PyObject* s_ct_key = nullptr;         // "__content_type__"
static PyObject* s_body_key2 = nullptr;      // body key2
static PyObject* s_ct_key2 = nullptr;        // content-type key2
static PyObject* s_async_di_tag = nullptr;   // "async_di"
static PyObject* s_deps_ran_key = nullptr;   // "__deps_ran__"
static PyObject* s_bg_key = nullptr;         // "__bg_tasks__"
static PyObject* s_serialize = nullptr;      // "serialize_python"
static PyObject* s_mw_tag = nullptr;         // "mw"
static PyObject* s_mdj = nullptr;            // "model_dump_json"
static PyObject* s_by_alias_kw = nullptr;    // "by_alias"
static PyObject* s_asdict = nullptr;         // "_asdict"
static PyObject* s_is_dc = nullptr;          // dataclass check
static PyObject* s_errors_str2 = nullptr;    // "errors"
static PyObject* s_url_key = nullptr;        // "url"
static PyObject* s_det_key = nullptr;        // "detail"
static PyObject* s_kw_filename = nullptr;    // "filename"
static PyObject* s_kw_file = nullptr;        // "file"
static PyObject* s_kw_ct = nullptr;          // "content_type"
static PyObject* s_mv_rve_cls = nullptr;     // RequestValidationError class
static PyObject* s_mv_body_kw = nullptr;     // "body" kw for RVE
static PyObject* s_rve_cls2 = nullptr;       // RequestValidationError class2
static PyObject* s_rve_body_kw = nullptr;    // "body" kw for RVE2

// Cached HTTP method strings — only 7 possible values, avoids per-request allocation
static PyObject* s_method_GET = nullptr;
static PyObject* s_method_POST = nullptr;
static PyObject* s_method_PUT = nullptr;
static PyObject* s_method_DELETE = nullptr;
static PyObject* s_method_PATCH = nullptr;
static PyObject* s_method_HEAD = nullptr;
static PyObject* s_method_OPTIONS = nullptr;

// Returns cached PyObject* (borrowed ref) for common methods, or creates a new one.
// For cached methods: returns borrowed ref (caller must NOT Py_DECREF).
// For unknown methods: returns new ref (caller must Py_DECREF).
// Sets *is_cached to true if a cached ref was returned.
static inline PyObject* get_cached_method(const char* data, size_t len, bool& is_cached) {
    is_cached = true;
    if (len == 3 && data[0] == 'G' && data[1] == 'E' && data[2] == 'T') return s_method_GET;
    if (len == 4 && data[0] == 'P' && data[1] == 'O' && data[2] == 'S' && data[3] == 'T') return s_method_POST;
    if (len == 3 && data[0] == 'P' && data[1] == 'U' && data[2] == 'T') return s_method_PUT;
    if (len == 6 && data[0] == 'D') return s_method_DELETE;
    if (len == 5 && data[0] == 'P' && data[1] == 'A') return s_method_PATCH;
    if (len == 4 && data[0] == 'H') return s_method_HEAD;
    if (len == 7 && data[0] == 'O') return s_method_OPTIONS;
    is_cached = false;
    return PyUnicode_FromStringAndSize(data, (Py_ssize_t)len);
}

void cleanup_cached_refs() {
    Py_CLEAR(s_http_exc_type);
    Py_CLEAR(s_astraapi_http_exc_type);
    Py_CLEAR(s_validation_exc_type);
    Py_CLEAR(s_resume_func);
    Py_CLEAR(s_request_body_to_args);
    Py_CLEAR(s_form_data_class);
    Py_CLEAR(s_upload_file_class);
    Py_CLEAR(g_str_write);
    Py_CLEAR(g_str_is_closing);
    Py_CLEAR(s_ensure_future);
    Py_CLEAR(s_kw_body_fields);
    Py_CLEAR(s_kw_received_body);
    Py_CLEAR(s_kw_embed);
    Py_CLEAR(s_detail_key_global);
    Py_CLEAR(s_loc_key_global);
    Py_CLEAR(s_url_key_global);
    Py_CLEAR(s_body_str_global);
    Py_CLEAR(s_include_url_key);
    Py_CLEAR(s_rh_key);
    Py_CLEAR(s_m_key);
    Py_CLEAR(s_p_key);
    Py_CLEAR(s_as_key);
    Py_CLEAR(s_ac_key);
    Py_CLEAR(s_validate_str);
    Py_CLEAR(s_fut_blocking);
    Py_CLEAR(s_async_tag);
    Py_CLEAR(s_stream_tag);
    Py_CLEAR(s_need_more_data);
    Py_CLEAR(s_method_GET);
    Py_CLEAR(s_method_POST);
    Py_CLEAR(s_method_PUT);
    Py_CLEAR(s_method_DELETE);
    Py_CLEAR(s_method_PATCH);
    Py_CLEAR(s_method_HEAD);
    Py_CLEAR(s_method_OPTIONS);
    Py_CLEAR(s_ct_type_key); Py_CLEAR(s_ct_loc_key); Py_CLEAR(s_ct_msg_key);
    Py_CLEAR(s_ct_input_key); Py_CLEAR(s_ct_mat_val); Py_CLEAR(s_ct_mat_msg);
    Py_CLEAR(s_ct_body_str); Py_CLEAR(s_body_key); Py_CLEAR(s_ct_key);
    Py_CLEAR(s_body_key2); Py_CLEAR(s_ct_key2); Py_CLEAR(s_async_di_tag);
    Py_CLEAR(s_deps_ran_key); Py_CLEAR(s_bg_key); Py_CLEAR(s_serialize);
    Py_CLEAR(s_mw_tag); Py_CLEAR(s_mdj); Py_CLEAR(s_by_alias_kw);
    Py_CLEAR(s_asdict); Py_CLEAR(s_is_dc); Py_CLEAR(s_errors_str2);
    Py_CLEAR(s_url_key); Py_CLEAR(s_det_key); Py_CLEAR(s_kw_filename);
    Py_CLEAR(s_kw_file); Py_CLEAR(s_kw_ct); Py_CLEAR(s_mv_rve_cls);
    Py_CLEAR(s_mv_body_kw); Py_CLEAR(s_rve_cls2); Py_CLEAR(s_rve_body_kw);
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
    if (!s_astraapi_http_exc_type) {
        PyRef mod(PyImport_ImportModule("astraapi.exceptions"));
        if (mod) s_astraapi_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    if (!s_validation_exc_type) {
        PyRef mod(PyImport_ImportModule("astraapi.exceptions"));
        if (mod) s_validation_exc_type = PyObject_GetAttrString(mod.get(), "RequestValidationError");
        else PyErr_Clear();
    }

    // Pre-import request_body_to_args (avoids 3-8ms lazy import on first body request)
    if (!s_request_body_to_args) {
        PyRef mod(PyImport_ImportModule("astraapi.dependencies.utils"));
        if (mod) s_request_body_to_args = PyObject_GetAttrString(mod.get(), "request_body_to_args");
        else PyErr_Clear();
    }
    // Pre-import FormData + UploadFile classes
    if (!s_form_data_class || !s_upload_file_class) {
        PyRef mod(PyImport_ImportModule("astraapi._datastructures_impl"));
        if (mod) {
            if (!s_form_data_class)
                s_form_data_class = PyObject_GetAttrString(mod.get(), "FormData");
            if (!s_upload_file_class)
                s_upload_file_class = PyObject_GetAttrString(mod.get(), "UploadFile");
        } else { PyErr_Clear(); }
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
    if (!s_loc_key_global) s_loc_key_global = PyUnicode_InternFromString("loc");
    if (!s_url_key_global) s_url_key_global = PyUnicode_InternFromString("url");
    if (!s_body_str_global) s_body_str_global = PyUnicode_InternFromString("body");
    if (!s_include_url_key) s_include_url_key = PyUnicode_InternFromString("include_url");

    // DI/async interned strings (promoted from lazy function-local statics)
    if (!s_rh_key) s_rh_key = PyUnicode_InternFromString("__raw_headers__");
    if (!s_m_key) s_m_key = PyUnicode_InternFromString("__method__");
    if (!s_p_key) s_p_key = PyUnicode_InternFromString("__path__");
    if (!s_as_key) s_as_key = PyUnicode_InternFromString("__auth_scheme__");
    if (!s_ac_key) s_ac_key = PyUnicode_InternFromString("__auth_credentials__");
    if (!s_validate_str) s_validate_str = PyUnicode_InternFromString("validate_python");
    if (!s_fut_blocking) s_fut_blocking = PyUnicode_InternFromString("_asyncio_future_blocking");
    if (!s_async_tag) s_async_tag = PyUnicode_InternFromString("async");
    if (!s_stream_tag) s_stream_tag = PyUnicode_InternFromString("stream");
    // Pre-intern attribute name strings for Response object detection (avoids C-str alloc per request)
    if (!s_attr_body_iterator) s_attr_body_iterator = PyUnicode_InternFromString("body_iterator");
    if (!s_attr_path) s_attr_path = PyUnicode_InternFromString("path");
    if (!s_attr_body) s_attr_body = PyUnicode_InternFromString("body");
    if (!s_attr_status_code) s_attr_status_code = PyUnicode_InternFromString("status_code");
    if (!s_attr_raw_headers) s_attr_raw_headers = PyUnicode_InternFromString("_raw_headers");
    if (!s_attr_raw_headers2) s_attr_raw_headers2 = PyUnicode_InternFromString("raw_headers");
    if (!s_attr_background) s_attr_background = PyUnicode_InternFromString("background");
    if (!s_attr_headers) s_attr_headers = PyUnicode_InternFromString("headers");

    // Pre-intern HTTP method strings (only 7 values — eliminates per-request allocation)
    if (!s_method_GET) s_method_GET = PyUnicode_InternFromString("GET");
    if (!s_method_POST) s_method_POST = PyUnicode_InternFromString("POST");
    if (!s_method_PUT) s_method_PUT = PyUnicode_InternFromString("PUT");
    if (!s_method_DELETE) s_method_DELETE = PyUnicode_InternFromString("DELETE");
    if (!s_method_PATCH) s_method_PATCH = PyUnicode_InternFromString("PATCH");
    if (!s_method_HEAD) s_method_HEAD = PyUnicode_InternFromString("HEAD");
    if (!s_method_OPTIONS) s_method_OPTIONS = PyUnicode_InternFromString("OPTIONS");

    // Cached return tuples
    if (!s_need_more_data) {
        PyObject* zero = PyLong_FromLong(0);
        if (zero) {
            s_need_more_data = PyTuple_Pack(2, zero, Py_None);
            Py_DECREF(zero);
        }
    }

    // Initialize promoted function-local statics
    if (!s_ct_type_key) s_ct_type_key = PyUnicode_InternFromString("type");
    if (!s_ct_loc_key) s_ct_loc_key = PyUnicode_InternFromString("loc");
    if (!s_ct_msg_key) s_ct_msg_key = PyUnicode_InternFromString("msg");
    if (!s_ct_input_key) s_ct_input_key = PyUnicode_InternFromString("input");
    if (!s_ct_mat_val) s_ct_mat_val = PyUnicode_InternFromString("model_attributes_type");
    if (!s_ct_mat_msg) s_ct_mat_msg = PyUnicode_InternFromString("Input should be a valid dictionary or object to extract fields from");
    if (!s_ct_body_str) s_ct_body_str = PyUnicode_InternFromString("body");
    if (!s_async_di_tag) s_async_di_tag = PyUnicode_InternFromString("async_di");
    if (!s_deps_ran_key) s_deps_ran_key = PyUnicode_InternFromString("__deps_ran__");
    if (!s_bg_key) s_bg_key = PyUnicode_InternFromString("__bg_tasks__");
    if (!s_serialize) s_serialize = PyUnicode_InternFromString("serialize_python");
    if (!s_mw_tag) s_mw_tag = PyUnicode_InternFromString("mw");
    if (!s_mdj) s_mdj = PyUnicode_InternFromString("model_dump_json");
    if (!s_by_alias_kw) s_by_alias_kw = PyUnicode_InternFromString("by_alias");
    if (!s_errors_str2) s_errors_str2 = PyUnicode_InternFromString("errors");
    if (!s_det_key) s_det_key = PyUnicode_InternFromString("detail");
    if (!s_kw_filename) s_kw_filename = PyUnicode_InternFromString("filename");
    if (!s_kw_file) s_kw_file = PyUnicode_InternFromString("file");
    if (!s_kw_ct) s_kw_ct = PyUnicode_InternFromString("content_type");
    if (!s_mv_body_kw) s_mv_body_kw = PyUnicode_InternFromString("body");
    if (!s_rve_body_kw) s_rve_body_kw = PyUnicode_InternFromString("body");

    PyErr_Clear();
    Py_RETURN_NONE;
}

// ── HTTPException type check helper ──────────────────────────────────────────
// Checks both starlette.exceptions.HTTPException and astraapi.exceptions.HTTPException
// since they are separate class hierarchies.
static bool is_http_exception(PyObject* exc_type) {
    if (!exc_type) return false;
    if (!s_http_exc_type) {
        PyRef mod(PyImport_ImportModule("starlette.exceptions"));
        if (mod) s_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    if (!s_astraapi_http_exc_type) {
        PyRef mod(PyImport_ImportModule("astraapi.exceptions"));
        if (mod) s_astraapi_http_exc_type = PyObject_GetAttrString(mod.get(), "HTTPException");
        else PyErr_Clear();
    }
    int r;
    if (s_http_exc_type) {
        r = PyObject_IsSubclass(exc_type, s_http_exc_type);
        if (r < 0) PyErr_Clear();
        if (r == 1) return true;
    }
    if (s_astraapi_http_exc_type) {
        r = PyObject_IsSubclass(exc_type, s_astraapi_http_exc_type);
        if (r < 0) PyErr_Clear();
        if (r == 1) return true;
    }
    return false;
}

static bool is_validation_exception(PyObject* exc_type) {
    if (!exc_type) return false;
    if (!s_validation_exc_type) {
        PyRef mod(PyImport_ImportModule("astraapi.exceptions"));
        if (mod) s_validation_exc_type = PyObject_GetAttrString(mod.get(), "RequestValidationError");
        else PyErr_Clear();
    }
    if (!s_validation_exc_type) return false;
    int r = PyObject_IsSubclass(exc_type, s_validation_exc_type);
    if (r < 0) PyErr_Clear();
    return r == 1;
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
    .tp_name = "_astraapi_core.InlineResult",
    .tp_basicsize = sizeof(InlineResultObject),
    .tp_dealloc = (destructor)InlineResult_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
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
    .tp_name = "_astraapi_core.MatchResult",
    .tp_basicsize = sizeof(MatchResultObject),
    .tp_dealloc = (destructor)MatchResult_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
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
    .tp_name = "_astraapi_core.ResponseData",
    .tp_basicsize = sizeof(ResponseDataObject),
    .tp_dealloc = (destructor)ResponseData_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
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
        new (&self->cors_config) AtomicSharedPtr<CorsConfig>();
        new (&self->trusted_host_config) AtomicSharedPtr<TrustedHostConfig>();
        new (&self->exception_handlers) std::unordered_map<uint16_t, PyObject*>();
        self->route_counter.store(0);
        self->counters.total_requests = 0;
        self->counters.active_requests = 0;
        self->counters.total_errors = 0;
        self->openapi_json_resp = nullptr;
        self->openapi_json_content = nullptr;
        self->docs_html_content = nullptr;
        self->redoc_html_content = nullptr;
        self->oauth2_redirect_html_content = nullptr;
        self->docs_html_resp = nullptr;
        self->redoc_html_resp = nullptr;
        self->oauth2_redirect_html_resp = nullptr;
        new (&self->openapi_url) std::string("/openapi.json");
        new (&self->docs_url) std::string("/docs");
        new (&self->redoc_url) std::string("/redoc");
        new (&self->oauth2_redirect_url) std::string("/docs/oauth2-redirect");
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
            Py_XDECREF(route.fast_spec->param_validator);
            Py_XDECREF(route.fast_spec->model_validate);
            Py_XDECREF(route.fast_spec->py_body_param_name);
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
    self->cors_config.~AtomicSharedPtr();
    self->trusted_host_config.~AtomicSharedPtr();
    self->exception_handlers.~unordered_map();
    Py_XDECREF(self->type_exception_handlers);
    Py_XDECREF(self->openapi_json_resp);
    Py_XDECREF(self->openapi_json_content);
    Py_XDECREF(self->docs_html_content);
    Py_XDECREF(self->redoc_html_content);
    Py_XDECREF(self->oauth2_redirect_html_content);
    Py_XDECREF(self->docs_html_resp);
    Py_XDECREF(self->redoc_html_resp);
    Py_XDECREF(self->oauth2_redirect_html_resp);
    self->openapi_url.~basic_string();
    self->docs_url.~basic_string();
    self->redoc_url.~basic_string();
    self->oauth2_redirect_url.~basic_string();
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
        buf_append(buf, sl.data, sl.len - 2);  // exclude \r\n (added with ct_pre below)
    } else {
        static const char prefix[] = "HTTP/1.1 ";
        buf_append(buf, prefix, sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf_append(buf, sc_buf, sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf_append(buf, reason, rlen);
    }

    // Content-Type header
    const char* ct_pre = "\r\ncontent-type: ";
    buf_append(buf, ct_pre, 16);
    size_t ct_len = strlen(content_type);
    buf_append(buf, content_type, ct_len);

    // Content-Length header
    const char* cl_pre = "\r\ncontent-length: ";
    buf_append(buf, cl_pre, 18);
    char cl_buf[20];
    int cl_n = fast_i64_to_buf(cl_buf, (long long)body_len);
    buf_append(buf, cl_buf, cl_n);

    // Connection + end of headers
    static const char end_hdr[] = "\r\nconnection: keep-alive\r\n\r\n";
    buf_append(buf, end_hdr, sizeof(end_hdr) - 1);

    // Body
    buf_append(buf, body, body_len);

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// Swagger UI HTML template
static const char SWAGGER_UI_HTML[] = R"(<!DOCTYPE html>
<html>
<head>
<title>%s</title>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" type="text/css" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css" >
</head>
<body>
<div id="swagger-ui">
</div>
<script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"> </script>
<script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-standalone-preset.js"> </script>
<script>
window.onload = function() {
const ui = SwaggerUIBundle({
"url": "%s",
"dom_id": "#swagger-ui",
presets: [
SwaggerUIBundle.presets.apis,
SwaggerUIBundle.SwaggerUIStandalonePreset
],
%s"oauth2RedirectUrl": window.location.origin + '%s',
})
window.ui = ui
}
</script>
</body>
</html>)";

// ReDoc HTML template
static const char REDOC_HTML[] = R"(<!DOCTYPE html>
<html>
<head>
<title>%s</title>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link href="https://fonts.googleapis.com/css?family=Montserrat:300,400,700|Roboto:300,400,700" rel="stylesheet">
</head>
<body>
<redoc spec-url='%s'></redoc>
<script src="https://cdn.jsdelivr.net/npm/redoc@2/bundles/redoc.standalone.js"> </script>
</body>
</html>)";

// OAuth2 redirect HTML (from Swagger UI oauth2-redirect.html)
static const char OAUTH2_REDIRECT_HTML[] = R"oauth2(<!doctype html>
<html lang="en-US">
<body onload="run()">
</body>
</html>
<script>
    'use strict';
    function run () {
        var oauth2 = window.opener.swaggerUIRedirectOauth2;
        var sentState = oauth2.state;
        var redirectUrl = oauth2.redirectUrl;
        var isValid, qp, arr;

        if (/code|token|error/.test(window.location.hash)) {
            qp = window.location.hash.substring(1).replace('?', '&');
        } else {
            qp = location.search.substring(1);
        }

        arr = qp.split("&");
        arr.forEach(function (v,i,_arr) { _arr[i] = '"' + v.replace('=', '":"') + '"';});
        qp = qp ? JSON.parse('{' + arr.join() + '}',
                function (key, value) {
                    return key ? decodeURIComponent(value) : value;
                }
        ) : {};

        isValid = qp.state === sentState;

        if ((
          oauth2.auth.schema.get("flow") === "accessCode" ||
          oauth2.auth.schema.get("flow") === "authorizationCode" ||
          oauth2.auth.schema.get("flow") === "authorization_code"
        ) && !oauth2.auth.code) {
            if (!isValid) {
                oauth2.errCb({
                    authId: oauth2.auth.name,
                    source: "auth",
                    level: "warning",
                    message: "Authorization may be unsafe, passed state was changed in server. The passed state wasn't returned from auth server."
                });
            }

            if (qp.code) {
                delete oauth2.state;
                oauth2.auth.code = qp.code;
                oauth2.callback({auth: oauth2.auth, redirectUrl: redirectUrl});
            } else {
                oauth2.errCb({authId: oauth2.auth.name, source: "auth", level: "error", message: "Authorization code was not found."});
            }
        } else if (!isValid) {
            oauth2.errCb({
                authId: oauth2.auth.name,
                source: "auth",
                level: "error",
                message: "Authorization may be unsafe, passed state was changed in server. The passed state wasn't returned from auth server or can't be parsed."
            });
        } else if (qp.access_token || qp.token || qp.id_token) {
            window.opener.swaggerUIRedirectOauth2.callback({auth: oauth2.auth, token: qp, redirectUrl: redirectUrl});
        } else {
            oauth2.errCb({authId: oauth2.auth.name, source: "auth", level: "error", message: "No access_token received from server."});
        }
        window.close();
    }
</script>)oauth2";

static PyObject* CoreApp_set_urls(CoreAppObject* self, PyObject* args) {
    // set_urls(openapi_url, docs_url, redoc_url, oauth2_redirect_url)
    const char *openapi_url = nullptr, *docs_url = nullptr, *redoc_url = nullptr, *oauth2_url = nullptr;
    if (!PyArg_ParseTuple(args, "zzzz", &openapi_url, &docs_url, &redoc_url, &oauth2_url)) return nullptr;
    if (openapi_url) self->openapi_url = openapi_url;
    if (docs_url) self->docs_url = docs_url;
    if (redoc_url) self->redoc_url = redoc_url;
    if (oauth2_url) self->oauth2_redirect_url = oauth2_url;
    Py_RETURN_NONE;
}

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
    // Store content for middleware
    Py_XDECREF(self->openapi_json_content);
    self->openapi_json_content = PyUnicode_FromStringAndSize(json_data, json_len);

    // Build Swagger UI HTML with substituted title, openapi_url, and oauth2_redirect_url
    {
        const std::string title = "AstraAPI - Swagger UI";
        const std::string& openapi_url = self->openapi_url;
        const std::string& oauth2_url = self->oauth2_redirect_url;
        // SWAGGER_UI_HTML has 4 %s placeholders: title, openapi_url, extra_params, oauth2_redirect_url
        const std::string& extra_params = self->swagger_ui_extra_params;
        std::string html;
        html.reserve(4096);
        int written = snprintf(nullptr, 0, SWAGGER_UI_HTML,
            title.c_str(), openapi_url.c_str(), extra_params.c_str(), oauth2_url.c_str());
        if (written > 0) {
            html.resize((size_t)written + 1);
            snprintf(html.data(), html.size(), SWAGGER_UI_HTML,
                title.c_str(), openapi_url.c_str(), extra_params.c_str(), oauth2_url.c_str());
            html.resize((size_t)written);
        }
        Py_XDECREF(self->docs_html_resp);
        self->docs_html_resp = build_static_response(
            200, "text/html; charset=utf-8", html.c_str(), html.size());
        Py_XDECREF(self->docs_html_content);
        self->docs_html_content = PyUnicode_FromStringAndSize(html.c_str(), (Py_ssize_t)html.size());
    }

    // Build ReDoc HTML with substituted title and openapi_url
    {
        const std::string title = "AstraAPI - ReDoc";
        const std::string& openapi_url = self->openapi_url;
        // REDOC_HTML has 2 %s placeholders: title, openapi_url
        std::string html;
        int written = snprintf(nullptr, 0, REDOC_HTML, title.c_str(), openapi_url.c_str());
        if (written > 0) {
            html.resize((size_t)written + 1);
            snprintf(html.data(), html.size(), REDOC_HTML, title.c_str(), openapi_url.c_str());
            html.resize((size_t)written);
        }
        Py_XDECREF(self->redoc_html_resp);
        self->redoc_html_resp = build_static_response(
            200, "text/html; charset=utf-8", html.c_str(), html.size());
        Py_XDECREF(self->redoc_html_content);
        self->redoc_html_content = PyUnicode_FromStringAndSize(html.c_str(), (Py_ssize_t)html.size());
    }

    // Build cached /docs/oauth2-redirect response
    Py_XDECREF(self->oauth2_redirect_html_resp);
    self->oauth2_redirect_html_resp = build_static_response(
        200, "text/html; charset=utf-8",
        OAUTH2_REDIRECT_HTML, sizeof(OAUTH2_REDIRECT_HTML) - 1);
    Py_XDECREF(self->oauth2_redirect_html_content);
    self->oauth2_redirect_html_content = PyUnicode_FromStringAndSize(
        OAUTH2_REDIRECT_HTML, (Py_ssize_t)(sizeof(OAUTH2_REDIRECT_HTML) - 1));

    Py_RETURN_NONE;
}

static PyObject* CoreApp_set_swagger_ui_parameters(CoreAppObject* self, PyObject* arg) {
    // arg = JSON string of extra SwaggerUIBundle parameters
    if (!PyUnicode_Check(arg)) { PyErr_SetString(PyExc_TypeError, "expected str"); return nullptr; }
    const char* params = PyUnicode_AsUTF8(arg);
    if (!params) return nullptr;
    self->swagger_ui_extra_params = params;
    // Rebuild docs HTML if already built
    if (self->docs_html_resp) {
        const std::string title = "AstraAPI - Swagger UI";
        const std::string& openapi_url = self->openapi_url;
        const std::string& oauth2_url = self->oauth2_redirect_url;
        const std::string& extra_params = self->swagger_ui_extra_params;
        std::string html;
        html.reserve(4096);
        int written = snprintf(nullptr, 0, SWAGGER_UI_HTML,
            title.c_str(), openapi_url.c_str(), extra_params.c_str(), oauth2_url.c_str());
        if (written > 0) {
            html.resize((size_t)written + 1);
            snprintf(html.data(), html.size(), SWAGGER_UI_HTML,
                title.c_str(), openapi_url.c_str(), extra_params.c_str(), oauth2_url.c_str());
            html.resize((size_t)written);
        }
        Py_XDECREF(self->docs_html_resp);
        self->docs_html_resp = build_static_response(200, "text/html; charset=utf-8", html.c_str(), html.size());
        Py_XDECREF(self->docs_html_content);
        self->docs_html_content = PyUnicode_FromStringAndSize(html.c_str(), (Py_ssize_t)html.size());
    }
    Py_RETURN_NONE;
}

static PyObject* CoreApp_next_route_id(CoreAppObject* self, PyObject*) {
    uint64_t id = self->route_counter.fetch_add(1, std::memory_order_relaxed);
    return PyLong_FromUnsignedLongLong(id);
}

static PyObject* CoreApp_record_request_start(CoreAppObject* self, PyObject*) {
    ++self->counters.total_requests;
    ++self->counters.active_requests;
    Py_RETURN_NONE;
}

static PyObject* CoreApp_record_request_end(CoreAppObject* self, PyObject*) {
    --self->counters.active_requests;
    Py_RETURN_NONE;
}

static PyObject* CoreApp_record_error(CoreAppObject* self, PyObject*) {
    ++self->counters.total_errors;
    Py_RETURN_NONE;
}

static PyObject* CoreApp_route_count(CoreAppObject* self, PyObject*) {
    // F-6: After freeze_routes(), routes are read-only — skip the lock.
    if (!self->routes_frozen.load(std::memory_order_acquire)) {
        std::shared_lock lock(self->routes_mutex);
        return PyLong_FromSsize_t((Py_ssize_t)self->routes.size());
    }
    return PyLong_FromSsize_t((Py_ssize_t)self->routes.size());
}

static PyObject* CoreApp_freeze_routes(CoreAppObject* self, PyObject*) {
    self->routes_frozen.store(true, std::memory_order_release);
    Py_RETURN_NONE;
}

static PyObject* CoreApp_get_metrics(CoreAppObject* self, PyObject*) {
    PyRef dict(PyDict_New());
    if (!dict) return nullptr;
    PyRef tr(PyLong_FromUnsignedLongLong(self->counters.total_requests));
    PyRef ar(PyLong_FromLongLong(self->counters.active_requests));
    PyRef te(PyLong_FromUnsignedLongLong(self->counters.total_errors));
    Py_ssize_t rc;
    if (!self->routes_frozen.load(std::memory_order_acquire)) {
        std::shared_lock lock(self->routes_mutex);
        rc = (Py_ssize_t)self->routes.size();
    } else {
        rc = (Py_ssize_t)self->routes.size();
    }
    PyRef rco(PyLong_FromSsize_t(rc));
    if (!tr || !ar || !te || !rco) return nullptr;
    if (PyDict_SetItemString(dict.get(), "total_requests", tr.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "active_requests", ar.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "total_errors", te.get()) < 0 ||
        PyDict_SetItemString(dict.get(), "route_count", rco.get()) < 0) return nullptr;
    return dict.release();
}

static PyObject* CoreApp_get_routes(CoreAppObject* self, PyObject*) {
    // F-6: Skip lock when routes are frozen (startup is done)
    const bool frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::optional<std::shared_lock<std::shared_mutex>> lock;
    if (!frozen) lock.emplace(self->routes_mutex);
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
        "has_body", "is_form", "is_multi_method", nullptr
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
    int has_body = 0, is_form = 0, is_multi_method_arg = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "sOOp|HOOOOpppOzzzppp", (char**)kwlist,
            &path, &methods_list, &endpoint, &is_coroutine,
            &status_code, &response_model_field, &response_class,
            &include, &exclude, &exclude_unset, &exclude_defaults, &exclude_none,
            &tags_list, &summary, &description, &operation_id,
            &has_body, &is_form, &is_multi_method_arg)) {
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
    route.is_multi_method = (bool)is_multi_method_arg;

    {
        std::unique_lock lock(self->routes_mutex);
        // Check if path already registered — merge method mask + store per-method endpoint
        auto existing = self->router.at(path, strlen(path));
        if (existing.has_value() && existing->route_index >= 0) {
            int eidx = existing->route_index;
            RouteInfo& er = self->routes[eidx];
            // Merge method mask; Python group dispatcher handles per-method routing
            er.method_mask |= route.method_mask;
            er.has_body = er.has_body || route.has_body;
            er.is_multi_method = true;
            // Clear fast spec body params so C++ does not require body for all methods
            if (er.fast_spec.has_value()) {
                auto& fs = *er.fast_spec;
                Py_XDECREF(fs.body_params); fs.body_params = nullptr;
                fs.has_body_params = false;
                fs.body_param_name = nullptr;
            }
            Py_DECREF(route.endpoint);
            route.endpoint = nullptr;
            return PyLong_FromUnsignedLongLong(er.route_id);
        }
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

    // F-6: Skip lock when routes are frozen (post-startup normal path)
    const bool frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::optional<std::shared_lock<std::shared_mutex>> lock;
    if (!frozen) lock.emplace(self->routes_mutex);

    auto match = self->router.at(path, strlen(path));
    if (!match) Py_RETURN_NONE;

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) Py_RETURN_NONE;

    const auto& route = self->routes[idx];

    if (route.method_mask) {
        uint8_t req_method = method_str_to_bit_ci(method, strlen(method));
        if (!(route.method_mask & req_method)) { Py_RETURN_NONE; }
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

    // F-6: Lock-free after freeze_routes()
    const bool frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::optional<std::shared_lock<std::shared_mutex>> lock;
    if (!frozen) lock.emplace(self->routes_mutex);

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

    const bool frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::optional<std::shared_lock<std::shared_mutex>> lock;
    if (!frozen) lock.emplace(self->routes_mutex);

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

    const bool frozen = self->routes_frozen.load(std::memory_order_acquire);
    std::optional<std::shared_lock<std::shared_mutex>> lock;
    if (!frozen) lock.emplace(self->routes_mutex);

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
            // Return original string so param_validator passes it to Pydantic.
            // This ensures validation error 'input' field shows the raw string
            // (e.g. "42") not the coerced int (42), matching standard AstraAPI.
            // Pydantic coerces "42" -> 42 on success; on failure shows "42".
            return PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size());
        }
        case TYPE_FLOAT: {
            // Return original string so that param_validator passes the original
            // string to Pydantic — ensuring validation error 'input' field shows
            // the raw string value ('2') not the coerced float (2.0), matching
            // standard AstraAPI behaviour.
            return PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size());
        }
        case TYPE_BOOL: {
            // Only accept the standard boolean string representations.
            // Any other string (e.g. 'foobar', '42') is returned as-is so that
            // param_validator produces a proper 422 validation error.
            if (val == "true" || val == "True" || val == "1" ||
                val == "yes"  || val == "Yes"  || val == "on" || val == "On") {
                Py_RETURN_TRUE;
            }
            if (val == "false" || val == "False" || val == "0" ||
                val == "no"    || val == "No"    || val == "off" || val == "Off") {
                Py_RETURN_FALSE;
            }
            // Invalid bool string — return as-is for proper 422 from param_validator
            return PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size());
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
    fprintf(stderr, "[astraapi-cpp] %s: %s\n", context, msg ? msg : "<unknown>");
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

static inline PyObject* make_consumed_true(CoreAppObject* self, size_t consumed) {
    self->last_consumed = (Py_ssize_t)consumed;
    Py_RETURN_TRUE;
}

// HELPER: Fast (consumed, obj) tuple — obj ref is stolen. Also sets last_consumed for Python.
static inline PyObject* make_consumed_obj(CoreAppObject* self, size_t consumed, PyObject* obj) {
    self->last_consumed = (Py_ssize_t)consumed;
    PyObject* c = PyLong_FromLongLong((long long)consumed);
    if (!c) { Py_DECREF(obj); return nullptr; }
    PyObject* t = PyTuple_New(2);
    if (!t) { Py_DECREF(c); Py_DECREF(obj); return nullptr; }
    PyTuple_SET_ITEM(t, 0, c);
    PyTuple_SET_ITEM(t, 1, obj);
    return t;
}

// HELPER: Build "mw" tuple for static routes when HTTP middleware is active
static inline PyObject* make_mw_tuple(CoreAppObject* self, const ParsedHttpRequest& req,
                                       PyObject* content_obj, int status_code) {
    static PyObject* s_mw_tag = nullptr;
    if (!s_mw_tag) s_mw_tag = PyUnicode_InternFromString("mw");
    Py_INCREF(s_mw_tag);
    Py_INCREF(content_obj);
    PyObject* ka = req.keep_alive ? Py_True : Py_False; Py_INCREF(ka);
    PyRef hdrs_list(PyList_New(req.header_count));
    if (hdrs_list) {
        for (int i = 0; i < req.header_count; i++) {
            const auto& hdr = req.headers[i];
            PyRef nb(PyBytes_FromStringAndSize(hdr.name.data, (Py_ssize_t)hdr.name.len));
            PyRef vb(PyBytes_FromStringAndSize(hdr.value.data, (Py_ssize_t)hdr.value.len));
            if (nb && vb) { PyRef p(PyTuple_Pack(2, nb.get(), vb.get())); if (p) PyList_SET_ITEM(hdrs_list.get(), i, p.release()); else { Py_INCREF(Py_None); PyList_SET_ITEM(hdrs_list.get(), i, Py_None); } }
            else { Py_INCREF(Py_None); PyList_SET_ITEM(hdrs_list.get(), i, Py_None); }
        }
    }
    bool mc = false;
    PyObject* method_str = get_cached_method(req.method.data, req.method.len, mc);
    PyRef path_str(PyUnicode_FromStringAndSize(req.path.data, (Py_ssize_t)req.path.len));
    PyRef mw_info(PyTuple_Pack(7, s_mw_tag, content_obj, get_cached_status(status_code), ka,
        hdrs_list ? hdrs_list.get() : Py_None,
        method_str ? method_str : Py_None,
        path_str ? path_str.get() : Py_None));
    if (!mc && method_str) Py_DECREF(method_str);
    Py_DECREF(ka);
    Py_DECREF(content_obj);
    if (mw_info) {
        --self->counters.active_requests;
        return make_consumed_obj(self, req.total_consumed, mw_info.release());
    }
    return nullptr;
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
        "body_params", "embed_body_fields", "dependant", "dep_solver", "param_validator", nullptr
    };

    Py_ssize_t route_index;
    const char* body_param_name = nullptr;
    PyObject* field_specs_list = Py_None;
    PyObject* body_params_obj = Py_None;
    int embed_body_fields = 0;
    PyObject* dependant_obj = Py_None;
    PyObject* dep_solver_obj = Py_None;
    PyObject* param_validator_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "n|zOOpOOO", (char**)kwlist,
            &route_index, &body_param_name, &field_specs_list,
            &body_params_obj, &embed_body_fields,
            &dependant_obj, &dep_solver_obj, &param_validator_obj)) {
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
    // Pre-intern body param name key — avoids PyUnicode_FromString on every POST request
    spec.py_body_param_name = body_param_name ? PyUnicode_InternFromString(body_param_name) : nullptr;
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
            PyObject* cu = PyDict_GetItemString(spec_dict, "convert_underscores");
            PyObject* isseq = PyDict_GetItemString(spec_dict, "is_sequence");

            fs.field_name = fn ? PyUnicode_AsUTF8(fn) : "";
            fs.alias = al ? PyUnicode_AsUTF8(al) : fs.field_name;
            fs.header_lookup_key = hlk ? PyUnicode_AsUTF8(hlk) : "";
            fs.location = loc ? (ParamLocation)PyLong_AsLong(loc) : LOC_QUERY;
            fs.type_tag = tt ? (ParamType)PyLong_AsLong(tt) : TYPE_STR;
            fs.required = req ? PyObject_IsTrue(req) : false;
            fs.convert_underscores = cu ? PyObject_IsTrue(cu) : true;
            fs.is_sequence = isseq ? PyObject_IsTrue(isseq) : false;
            PyObject* suo = PyDict_GetItemString(spec_dict, "seq_underscore_only");
            fs.seq_underscore_only = suo ? PyObject_IsTrue(suo) : false;
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

    // Param validator (Pydantic TypeAdapter-based constraint validation)
    spec.param_validator = nullptr;
    if (param_validator_obj != Py_None) {
        Py_INCREF(param_validator_obj);
        spec.param_validator = param_validator_obj;
    }

    route.fast_spec = std::move(spec);

    // Build O(1) lookup maps AFTER move — string_views must point into final location
    auto& final_spec = *route.fast_spec;
    for (size_t i = 0; i < final_spec.path_specs.size(); i++)
        final_spec.path_map[std::string_view(final_spec.path_specs[i].field_name)] = i;
    for (size_t i = 0; i < final_spec.query_specs.size(); i++) {
        const auto& fs = final_spec.query_specs[i];
        // If alias differs from field_name, only match by alias (not by Python name)
        if (!fs.alias.empty() && fs.alias != fs.field_name) {
            final_spec.query_map[std::string_view(fs.alias)] = i;
        } else {
            final_spec.query_map[std::string_view(fs.field_name)] = i;
        }
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
    // Build O(1) lookup set and wildcard flag from allow_origins
    for (const auto& o : config->allow_origins) {
        if (o == "*") {
            config->allow_any_origin = true;
        }
        config->allow_origins_set.insert(o);
    }
    config->allow_origin_regex = origin_regex ? std::optional<std::string>(origin_regex) : std::nullopt;
    if (config->allow_origin_regex.has_value()) {
        try {
            config->allow_origin_regex_compiled = std::regex(config->allow_origin_regex.value());
        } catch (const std::regex_error&) {
            PyErr_SetString(PyExc_ValueError, "Invalid CORS origin regex pattern");
            return nullptr;
        }
    } else {
        config->allow_origin_regex_compiled = std::nullopt;
    }
    config->allow_methods = list_to_vec(methods);
    config->allow_headers = list_to_vec(headers);
    config->allow_credentials = (bool)credentials;
    config->expose_headers = list_to_vec(expose);
    config->max_age = max_age;

    self->cors_config.store(config);
    self->cors_enabled = true;
    self->cors_ptr_cached = config.get();  // Raw pointer for hot-path — no atomic load

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
        if (h) {
            config->allowed_hosts.emplace_back(h);
            if (std::string_view(h) == "*") {
                config->allow_any_host = true;
            }
            config->allowed_hosts_set.emplace(h);
        }
    }
    self->trusted_host_config.store(config);
    self->trusted_host_enabled = !config->allow_any_host;
    self->th_ptr_cached = config.get();
    Py_RETURN_NONE;
}

static PyObject* CoreApp_check_trusted_host(CoreAppObject* self, PyObject* arg) {
    const char* host = PyUnicode_AsUTF8(arg);
    if (!host) return nullptr;

    auto config = self->trusted_host_config.load();
    if (!config) Py_RETURN_TRUE;
    if (config->allow_any_host) Py_RETURN_TRUE;

    if (config->allowed_hosts_set.count(std::string_view(host)) > 0) Py_RETURN_TRUE;
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

static PyObject* CoreApp_set_type_exception_handlers(CoreAppObject* self, PyObject* args) {
    PyObject* handlers_dict;
    if (!PyArg_ParseTuple(args, "O", &handlers_dict)) return nullptr;
    if (!PyDict_Check(handlers_dict)) { PyErr_SetString(PyExc_TypeError, "expected dict"); return nullptr; }
    Py_XDECREF(self->type_exception_handlers);
    Py_INCREF(handlers_dict);
    self->type_exception_handlers = handlers_dict;
    Py_RETURN_NONE;
}

static PyObject* CoreApp_set_https_redirect(CoreAppObject* self, PyObject* args) {
    int val = 0;
    if (!PyArg_ParseTuple(args, "p", &val)) return nullptr;
    self->https_redirect_enabled = (bool)val;
    Py_RETURN_NONE;
}

static PyObject* CoreApp_set_has_http_middleware(CoreAppObject* self, PyObject* args) {
    int val;
    if (!PyArg_ParseTuple(args, "p", &val)) return nullptr;
    self->has_http_middleware = (bool)val;
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
    if (cors->allow_any_origin) return true;
    // Transparent hash: looks up string_view directly — zero heap allocation
    if (cors->allow_origins_set.count(std::string_view(origin, origin_len)) > 0) return true;
    // Regex match (pre-compiled at configure time)
    if (cors->allow_origin_regex_compiled.has_value()) {
        try {
            if (std::regex_match(origin, origin + origin_len, cors->allow_origin_regex_compiled.value()))
                return true;
        } catch (...) {}
    }
    return false;
}

// Check for CRLF injection in origin strings (memchr = SIMD-accelerated on most platforms)
static inline bool contains_crlf(const char* s, size_t len) {
    return memchr(s, '\r', len) != nullptr || memchr(s, '\n', len) != nullptr;
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
    buf_append(buf, ACAO, sizeof(ACAO) - 1);
    // If wildcard and no credentials, use *; otherwise echo origin
    if (!cors->allow_credentials && cors->allow_origins.size() == 1 && cors->allow_origins[0] == "*") {
        buf.push_back('*');
    } else {
        buf_append(buf, origin, origin_len);
    }

    // Access-Control-Allow-Credentials
    if (cors->allow_credentials) {
        static const char ACAC[] = "\r\naccess-control-allow-credentials: true";
        buf_append(buf, ACAC, sizeof(ACAC) - 1);
    }

    // Access-Control-Expose-Headers
    if (!cors->expose_headers.empty()) {
        static const char ACEH[] = "\r\naccess-control-expose-headers: ";
        buf_append(buf, ACEH, sizeof(ACEH) - 1);
        for (size_t i = 0; i < cors->expose_headers.size(); i++) {
            if (i > 0) { buf.push_back(','); buf.push_back(' '); }
            const auto& h = cors->expose_headers[i];
            buf_append(buf, h.data(), h.size());
        }
    }

    // Vary: Origin (needed when not wildcard)
    if (cors->allow_credentials || cors->allow_origins.size() > 1 ||
        (cors->allow_origins.size() == 1 && cors->allow_origins[0] != "*")) {
        static const char VARY[] = "\r\nvary: Origin";
        buf_append(buf, VARY, sizeof(VARY) - 1);
    }

    return buf.size() - start;
}

// Build a full CORS preflight (OPTIONS) response
static PyObject* build_cors_preflight_response(
    const CorsConfig* cors, const char* origin, size_t origin_len, bool keep_alive,
    const char* req_headers = nullptr, size_t req_headers_len = 0)
{
    if (contains_crlf(origin, origin_len)) Py_RETURN_NONE;

    auto buf = acquire_buffer();
    // Pre-reserve for preflight response
    size_t estimate = 512;
    for (const auto& m : cors->allow_methods) estimate += m.size() + 2;
    for (const auto& h : cors->allow_headers) estimate += h.size() + 2;
    estimate += origin_len;
    buf.reserve(estimate);

    static const char STATUS[] = "HTTP/1.1 200 OK\r\naccess-control-allow-origin: ";
    buf_append(buf, STATUS, sizeof(STATUS) - 1);

    // Origin
    if (!cors->allow_credentials && cors->allow_origins.size() == 1 && cors->allow_origins[0] == "*") {
        buf.push_back('*');
    } else {
        buf_append(buf, origin, origin_len);
    }

    // Allow-Methods
    static const char ACAM[] = "\r\naccess-control-allow-methods: ";
    buf_append(buf, ACAM, sizeof(ACAM) - 1);
    if (cors->allow_methods.empty()) {
        static const char DEF_METHODS[] = "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD";
        buf_append(buf, DEF_METHODS, sizeof(DEF_METHODS) - 1);
    } else {
        for (size_t i = 0; i < cors->allow_methods.size(); i++) {
            if (i > 0) { buf.push_back(','); buf.push_back(' '); }
            const auto& m = cors->allow_methods[i];
            buf_append(buf, m.data(), m.size());
        }
    }

    // Allow-Headers: echo requested headers if allow_headers=["*"]
    if (!cors->allow_headers.empty()) {
        static const char ACAH[] = "\r\naccess-control-allow-headers: ";
        buf_append(buf, ACAH, sizeof(ACAH) - 1);
        bool is_wildcard = cors->allow_headers.size() == 1 && cors->allow_headers[0] == "*";
        if (is_wildcard && req_headers && req_headers_len > 0) {
            // Echo back the requested headers
            buf_append(buf, req_headers, req_headers_len);
        } else {
            for (size_t i = 0; i < cors->allow_headers.size(); i++) {
                if (i > 0) { buf.push_back(','); buf.push_back(' '); }
                const auto& h = cors->allow_headers[i];
                buf_append(buf, h.data(), h.size());
            }
        }
    }

    // Max-Age
    static const char AMA[] = "\r\naccess-control-max-age: ";
    buf_append(buf, AMA, sizeof(AMA) - 1);
    char age_buf[16];
    int age_n = fast_i64_to_buf(age_buf, cors->max_age);
    buf_append(buf, age_buf, age_n);

    // Credentials
    if (cors->allow_credentials) {
        static const char ACAC[] = "\r\naccess-control-allow-credentials: true";
        buf_append(buf, ACAC, sizeof(ACAC) - 1);
    }

    // Vary
    static const char VARY[] = "\r\nvary: Origin";
    buf_append(buf, VARY, sizeof(VARY) - 1);

    // Content-Length: 2 (body "OK") + Connection
    static const char CL2_KA[] = "\r\ncontent-length: 2\r\ncontent-type: text/plain\r\nconnection: keep-alive\r\n\r\nOK";
    static const char CL2_CLOSE[] = "\r\ncontent-length: 2\r\ncontent-type: text/plain\r\nconnection: close\r\n\r\nOK";
    if (keep_alive) {
        buf_append(buf, CL2_KA, sizeof(CL2_KA) - 1);
    } else {
        buf_append(buf, CL2_CLOSE, sizeof(CL2_CLOSE) - 1);
    }

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// ── Trusted host check ──────────────────────────────────────────────────────

static bool check_trusted_host_inline(const TrustedHostConfig* th, const char* host, size_t host_len) {
    if (!th) return true;  // No config = allow all
    if (th->allow_any_host) return true;
    std::string_view host_sv(host, host_len);
    // Strip port if present
    auto colon = host_sv.find(':');
    if (colon != std::string_view::npos) {
        host_sv = host_sv.substr(0, colon);
    }
    // Exact match
    if (th->allowed_hosts_set.count(host_sv) > 0) return true;
    // Wildcard match: check if any "*.suffix" entry matches
    for (const auto& allowed : th->allowed_hosts) {
        if (allowed.size() > 2 && allowed[0] == '*' && allowed[1] == '.') {
            std::string_view suffix(allowed.c_str() + 1, allowed.size() - 1); // ".example.com"
            if (host_sv.size() > suffix.size() &&
                host_sv.substr(host_sv.size() - suffix.size()) == suffix) return true;
            // Also allow bare domain (e.g. "example.com" matches "*.example.com")
            std::string_view bare(allowed.c_str() + 2, allowed.size() - 2); // "example.com"
            if (host_sv == bare) return true;
        }
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

// ── Shared ultra-fast JSON response buffer builder ────────────────────────────
// Used by both build_http_response_bytes and build_and_write_http_response.
static inline void build_fast_json_response_buf(
    std::vector<char>& buf, const CachedJsonPrefix& jp,
    const char* body_data, size_t body_len, bool keep_alive, bool head_only = false)
{
    char cl_buf[20];
    int cl_len = fast_i64_to_buf(cl_buf, (long long)body_len);  // real size for Content-Length
    static constexpr char SUFFIX_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
    static constexpr char SUFFIX_CLOSE[] = "\r\nconnection: close\r\n\r\n";
    const char* suffix = keep_alive ? SUFFIX_KA : SUFFIX_CLOSE;
    size_t suffix_len = keep_alive ? sizeof(SUFFIX_KA) - 1 : sizeof(SUFFIX_CLOSE) - 1;
    // HEAD: exclude body bytes but keep Content-Length accurate (RFC 7231 §4.3.2)
    size_t total = jp.len + cl_len + suffix_len + (head_only ? 0 : body_len);
    buf.resize(total);
    char* dst = buf.data();
    memcpy(dst, jp.data, jp.len); dst += jp.len;
    memcpy(dst, cl_buf, cl_len); dst += cl_len;
    memcpy(dst, suffix, suffix_len); dst += suffix_len;
    if (!head_only) { memcpy(dst, body_data, body_len); }
}

// ── Response builder with optional CORS + compression headers ────────────────

static PyObject* build_http_response_bytes(
    int status_code, const char* body_data, size_t body_len, bool keep_alive,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0,
    const char* content_encoding = nullptr, bool head_only = false,
    const char* extra_headers = nullptr, size_t extra_headers_len = 0)
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

    // Ultra-fast path: any status code with cached JSON prefix, no CORS, no compression
    // Covers ~99% of API responses. Single resize + 4 memcpy — zero buf_append calls.
    if (LIKELY(!cors && !content_encoding && !extra_headers &&
        status_code > 0 && status_code < 600 && s_json_prefixes[status_code].data)) {
        build_fast_json_response_buf(buf, s_json_prefixes[status_code],
                                     body_data, body_len, keep_alive, head_only);
        PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
        release_buffer(std::move(buf));
        return result;
    }

    if (status_code == 200 && !content_encoding) {
        buf_append(buf, HDR_200_JSON, sizeof(HDR_200_JSON) - 1);
    } else if (status_code == 200) {
        // 200 with CORS or compression
        buf_append(buf, HDR_200_JSON, sizeof(HDR_200_JSON) - 1);
    } else if (status_code > 0 && status_code < 600 && s_json_prefixes[status_code].data) {
        // Cached JSON prefix: "HTTP/1.1 XXX Reason\r\ncontent-type: application/json\r\ncontent-length: "
        const auto& jp = s_json_prefixes[status_code];
        buf_append(buf, jp.data, jp.len);
    } else {
        // Uncached status code — build dynamically
        static const char prefix[] = "HTTP/1.1 ";
        buf_append(buf, prefix, sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf_append(buf, sc_buf, sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf_append(buf, reason, rlen);
        static const char ct_hdr[] = "\r\ncontent-type: application/json\r\ncontent-length: ";
        buf_append(buf, ct_hdr, sizeof(ct_hdr) - 1);
    }

    // Content-Length value
    char sc[20];
    int n = fast_i64_to_buf(sc, (long long)body_len);
    buf_append(buf, sc, n);

    // Content-Encoding header (when compressed)
    if (content_encoding) {
        static const char ce_prefix[] = "\r\ncontent-encoding: ";
        buf_append(buf, ce_prefix, sizeof(ce_prefix) - 1);
        size_t ce_len = strlen(content_encoding);
        buf_append(buf, content_encoding, ce_len);
        // Also add Vary: Accept-Encoding for caching
        static const char vary_hdr[] = "\r\nvary: Accept-Encoding";
        buf_append(buf, vary_hdr, sizeof(vary_hdr) - 1);
    }

    // Extra headers (e.g. WWW-Authenticate from HTTPException.headers)
    // Each header should be in "name: value" form; we prepend \r\n
    if (extra_headers && extra_headers_len > 0) {
        buf_append(buf, "\r\n", 2);
        buf_append(buf, extra_headers, extra_headers_len);
    }

    // CORS headers (before connection header)
    if (cors && origin && origin_len > 0) {
        build_cors_headers(buf, cors, origin, origin_len);
    }

    // Connection header + end-of-headers (single insert)
    if (keep_alive) {
        buf_append(buf, CONN_KA, sizeof(CONN_KA) - 1);
    } else {
        buf_append(buf, CONN_CLOSE, sizeof(CONN_CLOSE) - 1);
    }

    // Body (skip for HEAD — Content-Length already set correctly above)
    if (!head_only) { buf_append(buf, body_data, body_len); }

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
            buf_append(buf, esc, 6);
        }
        else { buf.push_back((char)c); }
    }
}

static PyObject* build_http_error_response(int status_code, const char* message, bool keep_alive,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0,
    const char* extra_headers = nullptr, size_t extra_headers_len = 0) {
    // Build JSON error body: {"detail":"message"} with proper escaping
    auto body_buf = acquire_buffer();
    static constexpr char pre[] = "{\"detail\":\"";
    buf_append(body_buf, pre, sizeof(pre) - 1);
    json_escape_cstr(body_buf, message, strlen(message));
    static constexpr char post[] = "\"}";
    buf_append(body_buf, post, sizeof(post) - 1);

    PyObject* result = build_http_response_bytes(
        status_code, body_buf.data(), body_buf.size(), keep_alive, cors, origin, origin_len,
        nullptr, false, extra_headers, extra_headers_len);
    release_buffer(std::move(body_buf));
    return result;
}

// ── Extract HTTPException.headers dict into a raw header block string ─────
// Returns a std::string of "name: value\r\nname2: value2" pairs.
// Empty if exc_val has no headers or headers is None/empty.
static std::string extract_http_exc_headers(PyObject* exc_val) {
    std::string result;
    if (!exc_val) return result;
    PyRef hdrs(s_attr_headers
        ? PyObject_GetAttr(exc_val, s_attr_headers)
        : PyObject_GetAttrString(exc_val, "headers"));
    if (!hdrs || hdrs.get() == Py_None || !PyDict_Check(hdrs.get())) {
        PyErr_Clear();
        return result;
    }
    PyObject *key, *val;
    Py_ssize_t pos = 0;
    bool first = true;
    while (PyDict_Next(hdrs.get(), &pos, &key, &val)) {
        const char* kstr = PyUnicode_AsUTF8(key);
        const char* vstr = PyUnicode_AsUTF8(val);
        if (!kstr || !vstr) { PyErr_Clear(); continue; }
        if (!first) result += "\r\n";
        result += kstr;
        result += ": ";
        result += vstr;
        first = false;
    }
    return result;
}


static int write_to_transport(PyObject* transport, PyObject* data) {
    if (!transport || transport == Py_None) return -1;

    if (!g_str_write) g_str_write = PyUnicode_InternFromString("write");

    PyRef result(PyObject_CallMethodOneArg(transport, g_str_write, data));
    if (!result) {
        log_and_clear_pyerr("transport.write() failed");
        return -1;
    }
    return 0;
}

// ── Direct socket write — bypasses Python transport for small HTTP responses ──
// Uses platform_socket_write() (send() on all platforms) to write directly to
// the socket fd, avoiding transport.write() + PyBytes allocation overhead.
// Falls back to transport.write() for large responses or on partial/failed write.
// Matches existing WebSocket direct-fd pattern (ws_ring_buffer.cpp).
static int write_response_direct(int fd, PyObject* transport, PyObject* data) {
    if (fd >= 0 && PyBytes_Check(data)) {
        Py_ssize_t len = PyBytes_GET_SIZE(data);
        if (len > 0 && len <= 16384) {
            ssize_t sent = platform_socket_write(fd, PyBytes_AS_STRING(data), (size_t)len);
            if (sent == len) {
                platform_rearm_quickack(fd);  // next pipelined/keep-alive request ACK'd immediately
                return 0;  // Full write — bypassed Python entirely
            }
            if (sent > 0) {
                // Partial write: send remainder through Python transport
                PyRef remainder(PyBytes_FromStringAndSize(
                    PyBytes_AS_STRING(data) + sent, len - sent));
                if (remainder) return write_to_transport(transport, remainder.get());
                return -1;
            }
            // sent <= 0: EAGAIN/WSAEWOULDBLOCK — fall through to transport.write()
        }
    }
    return write_to_transport(transport, data);
}

// ── Build + write HTTP response — eliminates per-response PyBytes allocation ─
// For the ultra-fast path (no CORS, no compression, cached prefix): builds
// response directly in a pooled buffer and writes to socket fd, bypassing
// PyBytes entirely. Falls back to build_http_response_bytes for other cases.
static int build_and_write_http_response(
    int sock_fd, PyObject* transport,
    int status_code, const char* body_data, size_t body_len, bool keep_alive,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0,
    const char* content_encoding = nullptr, bool head_only = false)
{
    // Ultra-fast path: cached JSON prefix, no CORS, no compression
    if (LIKELY(!cors && !content_encoding &&
        status_code > 0 && status_code < 600 && s_json_prefixes[status_code].data)) {
        auto buf = acquire_buffer();
        buf.reserve(256 + (head_only ? 0 : body_len));
        build_fast_json_response_buf(buf, s_json_prefixes[status_code],
                                     body_data, body_len, keep_alive, head_only);

        // Direct write from buffer — skip PyBytes entirely
        if (sock_fd >= 0) {
            ssize_t sent = platform_socket_write(sock_fd, buf.data(), buf.size());
            if (sent == (ssize_t)buf.size()) {
                platform_rearm_quickack(sock_fd);
                release_buffer(std::move(buf));
                return 0;  // Zero Python allocations!
            }
            if (sent > 0) {
                PyRef rem(PyBytes_FromStringAndSize(buf.data() + sent, buf.size() - sent));
                release_buffer(std::move(buf));
                if (rem) return write_to_transport(transport, rem.get());
                return -1;
            }
            // sent <= 0: EAGAIN/WSAEWOULDBLOCK — fall through to transport
        }
        // Fallback: create PyBytes from buffer
        PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
        release_buffer(std::move(buf));
        if (!result) return -1;
        int rc = write_to_transport(transport, result);
        Py_DECREF(result);
        return rc;
    }

    // Slow path: CORS, compression, or uncached status — use existing builder
    PyObject* resp = build_http_response_bytes(status_code, body_data, body_len, keep_alive,
                                                cors, origin, origin_len, content_encoding, head_only);
    if (!resp) return -1;
    int rc = write_response_direct(sock_fd, transport, resp);
    Py_DECREF(resp);
    return rc;
}

// ── Fused JSON serialize + HTTP response + scatter-gather write ──────────────
// Eliminates serialize_to_json_pybytes PyBytes alloc + extra buffer cycle.
// Single buffer pool alloc for JSON body, headers built on stack, writev to socket.
static int serialize_json_and_write_response(
    int sock_fd, PyObject* transport,
    PyObject* json_obj, int status_code, bool keep_alive,
    const char* accept_encoding = nullptr, size_t ae_len = 0,
    const CorsConfig* cors = nullptr, const char* origin = nullptr, size_t origin_len = 0,
    bool head_only = false)
{
    // 1. Serialize JSON into buffer pool
    auto buf = acquire_buffer();
    if (UNLIKELY(write_json(json_obj, buf, 0) < 0)) {
        release_buffer(std::move(buf));
        return -1;
    }
    size_t body_len = buf.size();
    const char* body_data = buf.data();

    // 2. Optional compression
    const char* encoding = nullptr;
    std::vector<char> compressed;
    if (UNLIKELY(ae_len > 0 && body_len > 500)) {
        compressed = acquire_buffer();
        encoding = try_compress_inline(body_data, body_len, accept_encoding, ae_len, compressed);
        if (encoding) {
            body_data = compressed.data();
            body_len = compressed.size();
        }
    }

    // 3. Ultra-fast path: no CORS, no compression, cached prefix
    if (LIKELY(!cors && !encoding &&
        status_code > 0 && status_code < 600 && s_json_prefixes[status_code].data))
    {
        const auto& jp = s_json_prefixes[status_code];
        char header_buf[256];
        char cl_buf[20];
        int cl_len = fast_i64_to_buf(cl_buf, (long long)body_len);
        static constexpr char SUFFIX_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
        static constexpr char SUFFIX_CLOSE[] = "\r\nconnection: close\r\n\r\n";
        const char* suffix = keep_alive ? SUFFIX_KA : SUFFIX_CLOSE;
        size_t suffix_len = keep_alive ? sizeof(SUFFIX_KA) - 1 : sizeof(SUFFIX_CLOSE) - 1;

        // Build header on stack
        char* h = header_buf;
        memcpy(h, jp.data, jp.len); h += jp.len;
        memcpy(h, cl_buf, cl_len); h += cl_len;
        memcpy(h, suffix, suffix_len); h += suffix_len;
        size_t header_len = (size_t)(h - header_buf);

        // Scatter-gather write: [headers on stack] + [body in pool buffer]
        // HEAD: send headers only — Content-Length already reflects real body_len (RFC 7231)
        if (sock_fd >= 0) {
            if (UNLIKELY(head_only)) {
                // HEAD request: write headers only, no body bytes
                ssize_t sent = platform_socket_write(sock_fd, header_buf, header_len);
                if (sent == (ssize_t)header_len) {
                    release_buffer(std::move(buf));
                    if (!compressed.empty()) release_buffer(std::move(compressed));
                    return 0;
                }
                if (sent > 0) {
                    size_t rem_len = header_len - (size_t)sent;
                    PyRef rem(PyBytes_FromStringAndSize(header_buf + sent, (Py_ssize_t)rem_len));
                    if (rem) write_to_transport(transport, rem.get());
                    release_buffer(std::move(buf));
                    if (!compressed.empty()) release_buffer(std::move(compressed));
                    return 0;
                }
                // EAGAIN — fall through to transport
            } else {
                PlatformIoVec iov[2] = {
                    {header_buf, header_len},
                    {const_cast<char*>(body_data), body_len}
                };
                ssize_t total = (ssize_t)(header_len + body_len);
                ssize_t sent = platform_socket_writev(sock_fd, iov, 2);
                if (sent == total) {
                    platform_rearm_quickack(sock_fd);  // next keep-alive request ACK'd immediately
                    release_buffer(std::move(buf));
                    if (!compressed.empty()) release_buffer(std::move(compressed));
                    return 0;  // ZERO heap allocations!
                }
                if (sent > 0) {
                    // Partial write — build remainder as PyBytes, send via transport
                    if ((size_t)sent < header_len) {
                        size_t hdr_rem = header_len - (size_t)sent;
                        PyRef rem(PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)(hdr_rem + body_len)));
                        if (rem) {
                            char* dst = PyBytes_AS_STRING(rem.get());
                            memcpy(dst, header_buf + sent, hdr_rem);
                            memcpy(dst + hdr_rem, body_data, body_len);
                            write_to_transport(transport, rem.get());
                        }
                    } else {
                        size_t body_sent = (size_t)sent - header_len;
                        PyRef rem(PyBytes_FromStringAndSize(
                            body_data + body_sent, (Py_ssize_t)(body_len - body_sent)));
                        if (rem) write_to_transport(transport, rem.get());
                    }
                    release_buffer(std::move(buf));
                    if (!compressed.empty()) release_buffer(std::move(compressed));
                    return 0;
                }
                // EAGAIN — fall through to transport
            }
        }

        // Fallback: PyBytes for transport.write (HEAD: headers only; GET/POST: headers+body)
        if (UNLIKELY(head_only)) {
            PyRef hdr_only(PyBytes_FromStringAndSize(header_buf, (Py_ssize_t)header_len));
            if (hdr_only) write_to_transport(transport, hdr_only.get());
            release_buffer(std::move(buf));
            if (!compressed.empty()) release_buffer(std::move(compressed));
            return hdr_only ? 0 : -1;
        }
        PyRef full(PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)(header_len + body_len)));
        if (full) {
            char* dst = PyBytes_AS_STRING(full.get());
            memcpy(dst, header_buf, header_len);
            memcpy(dst + header_len, body_data, body_len);
            write_to_transport(transport, full.get());
        }
        release_buffer(std::move(buf));
        if (!compressed.empty()) release_buffer(std::move(compressed));
        return full ? 0 : -1;
    }

    // Slow path: CORS or uncached status — use existing response builder
    PyObject* resp = build_http_response_bytes(status_code, body_data, body_len, keep_alive,
                                                cors, origin, origin_len, encoding, head_only);
    release_buffer(std::move(buf));
    if (!compressed.empty()) release_buffer(std::move(compressed));
    if (!resp) return -1;
    int rc = write_response_direct(sock_fd, transport, resp);
    Py_DECREF(resp);
    return rc;
}

// ── HttpConnectionBuffer — replaces Python bytearray + memmove ──────────────
// Linear buffer with read/write offsets. Compact only when read_pos > 50% capacity.
// Eliminates: Python memoryview creation, slice ops, O(N) memmove per request.

class HttpConnectionBuffer {
    static constexpr size_t INITIAL_CAPACITY = 1024;   // 1KB initial — grows on demand
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
        // B-8: Only shrink when the buffer grew > 4x initial capacity.
        // Avoids a realloc syscall on every connection close for normal requests.
        // A 4KB high-water buffer stays allocated for reuse, saving allocator round-trips.
        if (capacity_ > INITIAL_CAPACITY * 4) {
            uint8_t* nb = static_cast<uint8_t*>(realloc(buf_, INITIAL_CAPACITY));
            if (nb) {
                buf_ = nb;
                capacity_ = INITIAL_CAPACITY;
            }
        }
    }

    // ── BufferedProtocol support: zero-copy receive ─────────────────────
    // ensure_writable() guarantees at least `n` bytes available for writing.
    // Returns false on allocation failure or overflow.
    bool ensure_writable(size_t n) {
        size_t used = write_pos_ - read_pos_;
        if (used + n > MAX_CAPACITY) return false;

        // Compact if read_pos > 50% of capacity
        if (read_pos_ > capacity_ / 2) {
            memmove(buf_, buf_ + read_pos_, used);
            write_pos_ = used;
            read_pos_ = 0;
        }

        // Grow if needed
        if (write_pos_ + n > capacity_) {
            size_t new_cap = capacity_;
            while (new_cap < write_pos_ + n) new_cap *= 2;
            if (new_cap > MAX_CAPACITY) new_cap = MAX_CAPACITY;
            uint8_t* nb = static_cast<uint8_t*>(realloc(buf_, new_cap));
            if (!nb) return false;
            buf_ = nb;
            capacity_ = new_cap;
        }
        return true;
    }

    uint8_t* write_ptr() { return buf_ + write_pos_; }
    size_t writable_size() const { return capacity_ - write_pos_; }

    void commit_write(size_t n) {
        write_pos_ += n;
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

// http_buf_get_write_buf: Return a writable memoryview into the buffer's unused space.
// Used by BufferedProtocol.get_buffer() to let asyncio write directly into our buffer.
// Args: capsule, sizehint (int)
PyObject* py_http_buf_get_write_buf(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_ssize_t sizehint;
    if (!PyArg_ParseTuple(args, "On", &capsule, &sizehint)) return nullptr;

    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }

    size_t hint = (sizehint > 0) ? (size_t)sizehint : 8192;
    if (hint < 4096) hint = 4096;  // Minimum useful read size
    if (!buf->ensure_writable(hint)) {
        PyErr_SetString(PyExc_MemoryError, "http_connection_buffer overflow");
        return nullptr;
    }

    return PyMemoryView_FromMemory(
        (char*)buf->write_ptr(), (Py_ssize_t)buf->writable_size(), PyBUF_WRITE);
}

// http_buf_commit_write: Advance write position after asyncio wrote into the buffer.
// Called from BufferedProtocol.buffer_updated(nbytes).
PyObject* py_http_buf_commit_write(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_ssize_t nbytes;
    if (!PyArg_ParseTuple(args, "On", &capsule, &nbytes)) return nullptr;

    auto* buf = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(capsule, HTTP_BUF_CAPSULE_NAME));
    if (!buf) {
        PyErr_SetString(PyExc_ValueError, "Invalid http_connection_buffer");
        return nullptr;
    }
    buf->commit_write((size_t)nbytes);
    Py_RETURN_NONE;
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

static PyObject* dispatch_one_request(
    CoreAppObject* self, const char* buf_data, Py_ssize_t buf_len,
    PyObject* transport, int sock_fd);

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
    if (nargs < 2 || nargs > 4) {
        PyErr_SetString(PyExc_TypeError, "handle_http requires 2-4 args (buffer, transport[, offset[, sock_fd]])");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];
    PyObject* transport = args[1];
    // OPT-7: Optional offset parameter eliminates memoryview slicing in Python loop
    Py_ssize_t offset = 0;
    if (nargs >= 3) {
        offset = PyLong_AsSsize_t(args[2]);
        if (offset < 0 && PyErr_Occurred()) return nullptr;
    }
    // OPT-DIRECT: Optional socket fd for direct write (bypasses Python transport)
    int sock_fd = -1;
    if (nargs >= 4) {
        sock_fd = (int)PyLong_AsLong(args[3]);
        if (sock_fd == -1 && PyErr_Occurred()) { PyErr_Clear(); sock_fd = -1; }
    }

    // Re-arm TCP_QUICKACK immediately — ensures the kernel ACKs the just-received
    // data without waiting 40ms (delayed ACK). One-shot on Linux; must be re-set
    // per data_received. Done here in C++ to eliminate the Python setsockopt
    // call overhead (~5-8 µs) that was previously in data_received().
    if (sock_fd >= 0) platform_rearm_quickack(sock_fd);

    // Fast-path: PyCapsule (HttpConnectionBuffer) — skip memoryview creation entirely
    char* buf_data;
    Py_ssize_t buf_len;
    Py_buffer view = {};
    bool have_view = false;

    if (PyCapsule_CheckExact(buffer_obj)) {
        auto* hb = static_cast<HttpConnectionBuffer*>(
            PyCapsule_GetPointer(buffer_obj, HTTP_BUF_CAPSULE_NAME));
        if (!hb) return nullptr;
        Py_ssize_t total = (Py_ssize_t)hb->size();
        if (offset > total) offset = total;
        buf_data = (char*)hb->data() + offset;
        buf_len = total - offset;
    } else {
        // Slow path: any buffer protocol object (bytes, bytearray, memoryview)
        if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) {
            return nullptr;
        }
        have_view = true;
        if (offset > view.len) offset = view.len;
        buf_data = (char*)view.buf + offset;
        buf_len = view.len - offset;
    }
    // RAII guard to release buffer on all return paths (no-op if capsule path)
    struct BufferGuard {
        Py_buffer* v; bool active;
        ~BufferGuard() { if (active) PyBuffer_Release(v); }
    } buf_guard{&view, have_view};

    return dispatch_one_request(self, buf_data, buf_len, transport, sock_fd);
}

static PyObject* dispatch_one_request(
    CoreAppObject* self, const char* buf_data, Py_ssize_t buf_len,
    PyObject* transport, int sock_fd)
{
    // ── Parse HTTP request ───────────────────────────────────────────────
    ParsedHttpRequest req = {};
    // Fast-path GET/HEAD parser: avoids llhttp state machine for simple requests
    int parse_result = 0;
    {
        const char* p = buf_data;
        bool fast_get  = (buf_len >= 16 && p[0]=='G' && p[1]=='E' && p[2]=='T' && p[3]==' ');
        bool fast_head = (!fast_get && buf_len >= 17 && p[0]=='H' && p[1]=='E' && p[2]=='A' && p[3]=='D' && p[4]==' ');
        if (fast_get || fast_head) {
            int mlen = fast_get ? 3 : 4;
            const char* uri = p + mlen + 1;
            const char* uri_end = uri;
            const char* end = p + buf_len;
            while (uri_end < end && *uri_end != ' ' && *uri_end != '\r') uri_end++;
            if (uri_end + 9 < end && *uri_end == ' ' &&
                uri_end[1]=='H' && uri_end[2]=='T' && uri_end[3]=='T' && uri_end[4]=='P' &&
                uri_end[5]=='/' && uri_end[7]=='.' && uri_end[9]=='\r') {
                const char* q = uri; while (q < uri_end && *q != '?') q++;
                req.path = StringView(uri, (size_t)(q - uri));
                if (q < uri_end) req.query_string = StringView(q+1, (size_t)(uri_end-q-1));
                req.full_uri = StringView(uri, (size_t)(uri_end - uri));
                req.method = fast_get ? StringView("GET",3) : StringView("HEAD",4);
                req.no_body = true; req.is_head = fast_head; req.keep_alive = true;
                const char* hdr = uri_end + 10;
                if (hdr < end && *hdr == '\n') hdr++;
                req.header_count = 0; bool found_end = false;
                while (hdr < end && req.header_count < MAX_HEADERS) {
                    if (hdr[0]=='\r' && hdr+1<end && hdr[1]=='\n') { hdr+=2; found_end=true; break; }
                    if (hdr[0]=='\n') { hdr++; found_end=true; break; }
                    const char* col=hdr; while(col<end && *col!=':' && *col!='\r' && *col!='\n') col++;
                    if (col>=end || *col!=':') break;
                    const char* v=col+1; while(v<end && *v==' ') v++;
                    const char* ve=v; while(ve<end && *ve!='\r' && *ve!='\n') ve++;
                    req.headers[req.header_count].name  = StringView(hdr,(size_t)(col-hdr));
                    req.headers[req.header_count].value = StringView(v,(size_t)(ve-v));
                    req.header_count++;
                    hdr=ve; if(hdr<end&&*hdr=='\r')hdr++; if(hdr<end&&*hdr=='\n')hdr++;
                }
                if (found_end) {
                    req.total_consumed = (size_t)(hdr - buf_data);
                    bool has_upgrade = false;
                    for (int i=0;i<req.header_count;i++) {
                        const auto& h=req.headers[i];
                        if (h.name.len==10 && (h.name.data[0]|32)=='c' && h.name.iequals("connection",10)) {
                            if (h.value.len>=7 && strncasecmp(h.value.data,"upgrade",7)==0) { has_upgrade=true; break; }
                            if (h.value.len>=5 && strncasecmp(h.value.data,"close",5)==0) req.keep_alive=false;
                        }
                    }
                    if (!has_upgrade) parse_result = 1;
                    // else: fall through to llhttp for WebSocket upgrade
                }
            }
        }
        if (parse_result == 0) parse_result = parse_http_request(buf_data, (size_t)buf_len, &req);
    }
    // Under connection pressure, force Connection: close to free slots
    if (self->force_close) req.keep_alive = false;

    if (parse_result == 0) {
        // Need more data — return None (zero allocations)
        Py_RETURN_NONE;
    }

    if (parse_result < 0) {
        // Parse error — send 400, return False (zero allocations)
        PyRef err_resp(build_http_error_response(400, "Bad Request", false));
        if (err_resp) write_response_direct(sock_fd, transport, err_resp.get());
        Py_RETURN_FALSE;
    }

    if (UNLIKELY(req.body_too_large)) {
        // Chunked body exceeded MAX_CHUNKED_BODY — send 413
        PyRef err_resp(build_http_error_response(413, "Payload Too Large", false));
        if (err_resp) write_response_direct(sock_fd, transport, err_resp.get());
        Py_RETURN_FALSE;
    }

    ++self->counters.total_requests;
    ++self->counters.active_requests;

    // ── Method flags (pre-computed by HTTP parser using llhttp enum) ──────
    // GET, HEAD, OPTIONS: body bytes were consumed by llhttp but discarded
    // (on_body is a no-op). req.body is empty, no body processing needed.
    // HEAD additionally requires stripping the response body while keeping headers.
    const bool is_head_method = req.is_head;
    const bool skip_body_parse = req.no_body;

    // Capture start time only if post-response hook is configured (skip ~25ns rdtsc otherwise)
    auto request_start_time = self->post_response_hook
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    // ── Extract Origin, Host, Accept-Encoding, Authorization headers ────
    StringView origin_sv;
    StringView host_sv;
    StringView accept_encoding_sv;
    StringView content_type_sv;
    StringView authorization_sv;
    {
        // Only look for origin if CORS is configured — cached bool avoids atomic shared_ptr load
        bool want_origin = self->cors_enabled;
        int found = 0;
        // Content-Type only needed for POST/PUT/PATCH body parsing — skip for GET/HEAD/OPTIONS
        const int need = want_origin ? (skip_body_parse ? 4 : 5) : (skip_body_parse ? 3 : 4);
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
                    // Skip content-type for body-less methods (GET, HEAD, OPTIONS)
                    if (!skip_body_parse && content_type_sv.empty() && fb == 'c' && hdr.name.iequals("content-type", 12))
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
    if (self->trusted_host_enabled && !host_sv.empty()) {
        if (!check_trusted_host_inline(self->th_ptr_cached, host_sv.data, host_sv.len)) {
            --self->counters.active_requests;
            PyRef resp(build_http_error_response(400, "Invalid host header", req.keep_alive));
            if (resp) write_response_direct(sock_fd, transport, resp.get());
            return make_consumed_true(self, req.total_consumed);
        }
    }

    // ── HTTPS redirect ──
    if (UNLIKELY(self->https_redirect_enabled)) {
        // Check if request is already HTTPS (via X-Forwarded-Proto or connection type)
        bool is_https = false;
        for (int i = 0; i < req.header_count; i++) {
            const auto& hdr = req.headers[i];
            if (hdr.name.len == 17 && memcmp(hdr.name.data, "x-forwarded-proto", 17) == 0) {
                is_https = (hdr.value.len >= 5 && memcmp(hdr.value.data, "https", 5) == 0);
                break;
            }
        }
        if (!is_https) {
            // Build redirect URL: https://{host}{path}{?query}
            std::string redirect_url;
            redirect_url.reserve(256);
            redirect_url += "https://";
            if (!host_sv.empty()) redirect_url.append(host_sv.data, host_sv.len);
            redirect_url.append(req.path.data, req.path.len);
            if (req.query_string.len > 0) {
                redirect_url += '?';
                redirect_url.append(req.query_string.data, req.query_string.len);
            }
            // Build 307 redirect response
            std::string resp_str;
            resp_str.reserve(512);
            resp_str += "HTTP/1.1 307 Temporary Redirect\r\nLocation: ";
            resp_str += redirect_url;
            resp_str += "\r\nContent-Length: 0\r\n";
            if (req.keep_alive) resp_str += "Connection: keep-alive\r\n";
            else resp_str += "Connection: close\r\n";
            resp_str += "\r\n";
            PyRef resp_bytes(PyBytes_FromStringAndSize(resp_str.c_str(), (Py_ssize_t)resp_str.size()));
            if (resp_bytes) write_response_direct(sock_fd, transport, resp_bytes.get());
            --self->counters.active_requests;
            return make_consumed_true(self, req.total_consumed);
        }
    }

    // ── CORS preflight (OPTIONS with Origin) ────────────────────────────
    const CorsConfig* cors_ptr = self->cors_enabled ? self->cors_ptr_cached : nullptr;
    bool has_cors = cors_ptr && !origin_sv.empty() && cors_origin_allowed(cors_ptr, origin_sv.data, origin_sv.len);

    if (UNLIKELY(has_cors && req.method.len == 7 && memcmp(req.method.data, "OPTIONS", 7) == 0)) {
        // Full CORS preflight response — entirely in C++, no route needed
        // Extract Access-Control-Request-Headers for echoing
        const char* acrh_data = nullptr; size_t acrh_len = 0;
        for (int i = 0; i < req.header_count; i++) {
            const auto& hdr = req.headers[i];
            if (hdr.name.len == 30 && hdr.name.iequals("access-control-request-headers", 30)) {
                acrh_data = hdr.value.data; acrh_len = hdr.value.len; break;
            }
        }
        --self->counters.active_requests;
        PyRef resp(build_cors_preflight_response(cors_ptr, origin_sv.data, origin_sv.len, req.keep_alive, acrh_data, acrh_len));
        if (resp) write_response_direct(sock_fd, transport, resp.get());
        return make_consumed_true(self, req.total_consumed);
    }

    // ── Serve /openapi.json, /docs, /redoc (pre-built responses) ──────
    if (req.path.len > 1) {
        std::string_view path_sv(req.path.data, req.path.len);
        if (self->openapi_json_resp && path_sv == self->openapi_url) {
            if (self->has_http_middleware) {
                if (self->openapi_json_content) return make_mw_tuple(self, req, self->openapi_json_content, 200);
            }
            --self->counters.active_requests;
            write_response_direct(sock_fd, transport, self->openapi_json_resp);
            return make_consumed_true(self, req.total_consumed);
        } else if (self->docs_html_resp && path_sv == self->docs_url) {
            if (self->has_http_middleware) {
                if (self->docs_html_content) return make_mw_tuple(self, req, self->docs_html_content, 200);
            }
            --self->counters.active_requests;
            write_response_direct(sock_fd, transport, self->docs_html_resp);
            return make_consumed_true(self, req.total_consumed);
        } else if (self->oauth2_redirect_html_resp && path_sv == self->oauth2_redirect_url) {
            if (self->has_http_middleware) {
                if (self->oauth2_redirect_html_content) return make_mw_tuple(self, req, self->oauth2_redirect_html_content, 200);
            }
            --self->counters.active_requests;
            write_response_direct(sock_fd, transport, self->oauth2_redirect_html_resp);
            return make_consumed_true(self, req.total_consumed);
        } else if (self->redoc_html_resp && path_sv == self->redoc_url) {
            if (self->has_http_middleware) {
                if (self->redoc_html_content) return make_mw_tuple(self, req, self->redoc_html_content, 200);
            }
            --self->counters.active_requests;
            write_response_direct(sock_fd, transport, self->redoc_html_resp);
            return make_consumed_true(self, req.total_consumed);
        }
    }

    // ── WebSocket upgrade detection ────────────────────────────────────
    if (UNLIKELY(req.upgrade)) {
        // Extract Sec-WebSocket-Key header
        StringView ws_key;
        for (int i = 0; i < req.header_count; i++) {
            if (req.headers[i].name.iequals("sec-websocket-key", 17)) {
                ws_key = req.headers[i].value;
                break;
            }
        }

        if (ws_key.len > 0) {
            // Extract Sec-WebSocket-Extensions header (for permessage-deflate negotiation)
            StringView ws_extensions;
            StringView ws_subprotocol;
            for (int i = 0; i < req.header_count; i++) {
                if (req.headers[i].name.iequals("sec-websocket-extensions", 24))
                    ws_extensions = req.headers[i].value;
                else if (req.headers[i].name.iequals("sec-websocket-protocol", 22))
                    ws_subprotocol = req.headers[i].value;
            }
            // Use ext-aware upgrade: negotiates permessage-deflate if client requests it,
            // ensures RSV bits are explicitly refused when compression is NOT agreed —
            // prevents clients sending RSV1=1 frames that would trigger protocol error 1002.
            auto upgrade_resp = ws_build_upgrade_response_ext(
                ws_key.data, ws_key.len,
                ws_extensions.data, ws_extensions.len,
                ws_subprotocol.data, ws_subprotocol.len,
                nullptr);  // deflate_out=nullptr: never negotiate compression (no inflate ctx)
            PyRef resp_bytes(PyBytes_FromStringAndSize(upgrade_resp.data(), (Py_ssize_t)upgrade_resp.size()));
            if (resp_bytes) write_response_direct(sock_fd, transport, resp_bytes.get());

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

            --self->counters.active_requests;
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
                // Must set last_consumed so handle_http_append_and_dispatch /
                // handle_http_batch_v2 advance the HTTP buffer past the upgrade request.
                self->last_consumed = (Py_ssize_t)req.total_consumed;
                return PyTuple_Pack(2, consumed.get(), ws_info.get());
            }

            // No route found for WebSocket — consume request and return sentinel
            self->last_consumed = (Py_ssize_t)req.total_consumed;
            Py_INCREF(Py_True);
            return PyTuple_Pack(2, consumed.get(), Py_True);
        }
    }

    // ── Route matching ───────────────────────────────────────────────────
    // Skip lock when routes are frozen (after startup) — atomic check first.
    // Auto-freeze on first request if not already frozen to eliminate lock contention.
    if (UNLIKELY(!self->routes_frozen.load(std::memory_order_acquire))) {
        std::unique_lock wlock(self->routes_mutex);
        self->routes_frozen.store(true, std::memory_order_release);
    }

    auto match = self->router.at(req.path.data, req.path.len);
    if (UNLIKELY(!match)) {
        // ── Trailing slash redirect: try with/without '/' ───────────
        // Use stack buffer to avoid heap allocation for path probe
        char alt_buf[512];
        size_t alt_len = req.path.len;
        bool do_redirect_check = (alt_len > 0 && alt_len < sizeof(alt_buf) - 1);
        if (do_redirect_check) {
            std::memcpy(alt_buf, req.path.data, alt_len);
            if (alt_buf[alt_len - 1] == '/') {
                alt_len--;  // try without trailing slash
            } else {
                alt_buf[alt_len++] = '/';  // try with trailing slash
            }
            alt_buf[alt_len] = '\0';
        }
        auto alt_match = (do_redirect_check && self->redirect_slashes) ? self->router.at(alt_buf, alt_len) : std::nullopt;
        if (alt_match) {
            --self->counters.active_requests;
            // Build 307 redirect response using resize+memcpy
            // Build absolute URL using Host header + scheme
            std::string_view host_sv;
            bool is_https = false;
            for (int hi = 0; hi < req.header_count; hi++) {
                const auto& hdr = req.headers[hi];
                if (hdr.name.len == 4 && strncasecmp(hdr.name.data, "host", 4) == 0) {
                    host_sv = std::string_view(hdr.value.data, hdr.value.len);
                } else if (hdr.name.len == 17 && strncasecmp(hdr.name.data, "x-forwarded-proto", 17) == 0) {
                    is_https = (hdr.value.len >= 5 && strncasecmp(hdr.value.data, "https", 5) == 0);
                }
            }
            auto buf = acquire_buffer();
            buf.reserve(256);
            static const char redir[] = "HTTP/1.1 307 Temporary Redirect\r\nlocation: ";
            constexpr size_t redir_sz = sizeof(redir) - 1;
            size_t old = buf.size();
            buf.resize(old + redir_sz);
            std::memcpy(buf.data() + old, redir, redir_sz);
            // Prepend scheme+host if host header is present and not a test placeholder
            bool is_test_host = (host_sv == "testserver" || host_sv == "localhost" || host_sv.find("127.0.0.1") == 0 || host_sv.find("::1") == 0);
            if (!host_sv.empty() && !is_test_host) {
                const char* scheme = is_https ? "https://" : "http://";
                size_t scheme_len = is_https ? 8 : 7;
                old = buf.size();
                buf.resize(old + scheme_len + host_sv.size());
                std::memcpy(buf.data() + old, scheme, scheme_len);
                std::memcpy(buf.data() + old + scheme_len, host_sv.data(), host_sv.size());
            }
            old = buf.size();
            buf.resize(old + alt_len);
            std::memcpy(buf.data() + old, alt_buf, alt_len);
            // Add query string if present
            if (!req.query_string.empty()) {
                buf.push_back('?');
                old = buf.size();
                buf.resize(old + req.query_string.len);
                std::memcpy(buf.data() + old, req.query_string.data, req.query_string.len);
            }
            static const char redir_end[] = "\r\ncontent-length: 0\r\nconnection: keep-alive\r\n\r\n";
            constexpr size_t redir_end_sz = sizeof(redir_end) - 1;
            old = buf.size();
            buf.resize(old + redir_end_sz);
            std::memcpy(buf.data() + old, redir_end, redir_end_sz);
            PyRef resp(PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size()));
            release_buffer(std::move(buf));
            if (resp) write_response_direct(sock_fd, transport, resp.get());
            return make_consumed_true(self, req.total_consumed);
        }

        --self->counters.active_requests;
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_response_direct(sock_fd, transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 404, request_start_time);
        return make_consumed_true(self, req.total_consumed);
    }

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) {
        --self->counters.active_requests;
        PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_response_direct(sock_fd, transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 404, request_start_time);
        return make_consumed_true(self, req.total_consumed);
    }

    const auto& route = self->routes[idx];

    // ── Method check (O(1) bitmask) ────────────────────────────────────
    if (LIKELY(route.method_mask)) {
        uint8_t req_method = method_str_to_bit(req.method.data, req.method.len);
        if (UNLIKELY(!(route.method_mask & req_method))) {
            --self->counters.active_requests;
            PyRef resp(build_http_error_response(405, "Method Not Allowed", req.keep_alive,
                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
            if (resp) write_response_direct(sock_fd, transport, resp.get());
            fire_post_response_hook(self, req.method.data, req.method.len,
                                    req.path.data, req.path.len, 405, request_start_time);
            return make_consumed_true(self, req.total_consumed);
        }
    }

    // ── Rate limiting (C++ native, per client IP) — sharded for low contention
    if (UNLIKELY(self->rate_limit_enabled && !self->current_client_ip.empty())) {
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t window_ns = (int64_t)self->rate_limit_window_seconds * 1'000'000'000LL;
        size_t shard_idx = self->current_shard_idx;  // cached at connection time
        auto& shard = self->rate_limit_shards[shard_idx];
        std::lock_guard<std::mutex> rl_lock(shard.mutex);
        // OPT-3.7: Use transparent find with string_view to avoid per-request
        // std::string copy+hash. Only insert (which needs a key copy) on first miss.
        std::string_view ip_sv(self->current_client_ip);
        auto it = shard.counters.find(ip_sv);
        if (it == shard.counters.end()) {
            it = shard.counters.try_emplace(self->current_client_ip).first;
        }
        auto& entry = it->second;
        if (now_ns - entry.window_start_ns > window_ns) {
            entry.count = 1;
            entry.window_start_ns = now_ns;
        } else {
            entry.count++;
        }
        if (entry.count > self->rate_limit_max_requests) {
            // Unlock route lock before writing response
            --self->counters.active_requests;
            PyRef resp(build_http_error_response(429, "Rate limit exceeded", req.keep_alive,
                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
            if (resp) write_response_direct(sock_fd, transport, resp.get());
            return make_consumed_true(self, req.total_consumed);
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

    if (UNLIKELY(!route.fast_spec)) {
        --self->counters.active_requests;
        PyRef resp(build_http_error_response(500, "Route not configured", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_response_direct(sock_fd, transport, resp.get());
        return make_consumed_true(self, req.total_consumed);
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


    // RAII guard — auto-DECREF response_model_local on any return path
    struct PyObjGuard { PyObject* p; ~PyObjGuard() { Py_XDECREF(p); } };
    PyObjGuard model_guard{response_model_local};

    const auto& spec = *spec_ptr;

    // ── OPT: Skip kwargs for zero-param endpoints (e.g. GET /) ──────────
    // NOTE: is_form_local must also force kwargs creation — form routes don't
    // register body_params (has_body_params_local=false) but the form parser
    // writes field values directly into kwargs via PyDict_SetItem. Without this,
    // kwargs is nullptr for form-only routes → segfault in PyDict_SetItem.
    bool needs_kwargs = (match->param_count > 0) ||
        spec.has_query_params || spec.has_header_params ||
        spec.has_cookie_params || has_body_params_local ||
        spec.has_dependencies || !req.query_string.empty() ||
        is_form_local || route.is_multi_method;

    PyRef kwargs(needs_kwargs ? PyDict_New() : nullptr);
    if (needs_kwargs && !kwargs) return nullptr;

    // Inject __method__ and __body__ for multi-method routes (group dispatcher needs them)
    if (route.is_multi_method && kwargs) {
        bool mc = false;
        PyObject* ms = get_cached_method(req.method.data, req.method.len, mc);
        if (ms) { PyDict_SetItem(kwargs.get(), s_m_key, ms); if (!mc) Py_DECREF(ms); }
        // Pass raw body bytes so group dispatcher can parse body params
        if (req.body.len > 0) {
            PyRef body_bytes(PyBytes_FromStringAndSize(req.body.data, (Py_ssize_t)req.body.len));
            if (body_bytes) {
                if (!s_body_key) s_body_key = PyUnicode_InternFromString("__body__");
                PyDict_SetItem(kwargs.get(), s_body_key, body_bytes.get());
            }
        }
        // Pass content-type header
        for (int hi = 0; hi < req.header_count; hi++) {
            if (req.headers[hi].name.len == 12 &&
                strncasecmp(req.headers[hi].name.data, "content-type", 12) == 0) {
                PyRef ct(PyUnicode_FromStringAndSize(req.headers[hi].value.data, (Py_ssize_t)req.headers[hi].value.len));
                if (ct) {
                    if (!s_ct_key) s_ct_key = PyUnicode_InternFromString("__content_type__");
                    PyDict_SetItem(kwargs.get(), s_ct_key, ct.get());
                }
                break;
            }
        }
    }

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

    // ── Query parameters (O(1) hash map lookup + percent-decoding) ─────
    if (spec.has_query_params && !req.query_string.empty()) {
        const char* p = req.query_string.data;
        const char* end = p + req.query_string.len;
        // Scratch buffers — reused across iterations (eliminates 2N allocs)
        std::string decoded_key, decoded_val;
        decoded_key.reserve(64);
        decoded_val.reserve(256);
        while (p < end) {
            const char* key_start = p;
            const char* eq = nullptr;
            while (p < end && *p != '&') {
                if (*p == '=' && !eq) eq = p;
                p++;
            }
            if (eq) {
                const char* raw_key = key_start;
                size_t raw_key_len = eq - key_start;
                const char* raw_val = eq + 1;
                size_t raw_val_len = p - eq - 1;

                // Single-pass decode: skip if no encoding needed (common case)
                std::string_view key_sv;
                if (percent_decode_into_if_needed(decoded_key, raw_key, raw_key_len)) {
                    key_sv = std::string_view(decoded_key);
                } else {
                    key_sv = std::string_view(raw_key, raw_key_len);
                }

                auto qit = spec.query_map.find(key_sv);
                if (qit != spec.query_map.end()) {
                    const auto& fs = spec.query_specs[qit->second];
                    // Single-pass percent-decode value before coercion
                    std::string_view val_sv;
                    if (percent_decode_into_if_needed(decoded_val, raw_val, raw_val_len)) {
                        val_sv = std::string_view(decoded_val);
                    } else {
                        val_sv = std::string_view(raw_val, raw_val_len);
                    }
                    PyObject* py_val = coerce_param(val_sv, fs.type_tag);
                    if (!py_val) return nullptr;
                    // Multi-value: accumulate repeated keys into a list
                    // (handles List[T], frozenset[T], set[T] query params)
                    PyObject* existing = PyDict_GetItem(kwargs.get(), fs.py_field_name);
                    if (existing && PyList_Check(existing)) {
                        PyList_Append(existing, py_val);
                    } else if (existing) {
                        // Second occurrence — convert to list
                        PyRef lst(PyList_New(2));
                        if (lst) {
                            Py_INCREF(existing);
                            PyList_SET_ITEM(lst.get(), 0, existing);
                            Py_INCREF(py_val);
                            PyList_SET_ITEM(lst.get(), 1, py_val);
                            PyDict_SetItem(kwargs.get(), fs.py_field_name, lst.get());
                        }
                    } else {
                        PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val);
                    }
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
                if (hdr.name.len > 255) continue;  // Skip oversized header names
                char norm_buf[256];
                size_t norm_len = hdr.name.len;
                for (size_t j = 0; j < norm_len; j++) {
                    norm_buf[j] = s_header_norm[(unsigned char)hdr.name.data[j]];
                }
                std::string_view normalized(norm_buf, norm_len);
                auto hit = spec.header_map.find(normalized);
                if (hit != spec.header_map.end()) {
                    const auto& fs = spec.header_specs[hit->second];
                    {
                        // Check if header has dashes (needed for convert_underscores and seq_underscore_only)
                        bool hdr_has_dash = false;
                        if (!fs.convert_underscores || fs.seq_underscore_only) {
                            for (size_t j = 0; j < hdr.name.len; j++) {
                                if (hdr.name.data[j] == '-') { hdr_has_dash = true; break; }
                            }
                        }
                        // convert_underscores=False + scalar: skip dash headers
                        if (!fs.convert_underscores && hdr_has_dash && !fs.is_sequence) continue;
                        PyRef py_val(PyUnicode_FromStringAndSize(hdr.value.data, hdr.value.len));
                        if (py_val) {
                            // seq_underscore_only: only collect into list if header has underscores
                            bool do_collect = fs.is_sequence && !(fs.seq_underscore_only && hdr_has_dash);
                            if (do_collect) {
                                // Collect multiple values into a list
                                PyObject* existing = PyDict_GetItem(kwargs.get(), fs.py_field_name);
                                if (existing && PyList_Check(existing)) {
                                    PyList_Append(existing, py_val.get());
                                } else {
                                    PyRef lst(PyList_New(1));
                                    if (lst) {
                                        Py_INCREF(py_val.get());
                                        PyList_SET_ITEM(lst.get(), 0, py_val.get());
                                        PyDict_SetItem(kwargs.get(), fs.py_field_name, lst.get());
                                    }
                                }
                            } else {
                                PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Fill defaults for missing params ─────────────────────────────────
    auto fill_defaults_http = [&](const std::vector<FieldSpec>& specs) {
        for (const auto& fs : specs) {
            // PyDict_SetDefault: single hash+lookup — sets only if key absent
            if (fs.default_value) {
                PyDict_SetDefault(kwargs.get(), fs.py_field_name, fs.default_value);
            }
        }
    };
    fill_defaults_http(spec.query_specs);
    fill_defaults_http(spec.header_specs);
    fill_defaults_http(spec.cookie_specs);

    // ── Param validation (Pydantic TypeAdapter constraints + required check) ─
    if (spec.param_validator && needs_kwargs) {
        PyRef pv_result(PyObject_CallOneArg(spec.param_validator, kwargs.get()));
        if (pv_result && PyTuple_Check(pv_result.get()) && PyTuple_GET_SIZE(pv_result.get()) >= 2) {
            PyObject* pv_values = PyTuple_GET_ITEM(pv_result.get(), 0);  // borrowed
            PyObject* pv_errors = PyTuple_GET_ITEM(pv_result.get(), 1);  // borrowed
            if (pv_errors && PyList_Check(pv_errors) && PyList_GET_SIZE(pv_errors) > 0) {
                // Return 422 with validation errors
                PyRef err_dict(PyDict_New());
                if (err_dict) {
                    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                    PyDict_SetItem(err_dict.get(), s_detail_key_global, pv_errors);
                    PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                    if (err_json) {
                        char* ej_data; Py_ssize_t ej_len;
                        PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                        build_and_write_http_response(sock_fd, transport, 422, ej_data, (size_t)ej_len, req.keep_alive,
                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                    }
                }
                Py_DECREF(endpoint_local);
                Py_XDECREF(body_params_local);
                --self->counters.active_requests;
                ++self->counters.total_errors;
                return make_consumed_true(self, req.total_consumed);
            } else if (pv_values && PyDict_Check(pv_values) && PyDict_GET_SIZE(pv_values) > 0) {
                // Update kwargs with coerced/validated values
                PyDict_Update(kwargs.get(), pv_values);
            }
        } else {
            PyErr_Clear();  // Don't break dispatch on validator error
        }
    }

    // ── Body parsing: JSON or Form data ────────────────────────────────────
    // Skip entirely for GET, HEAD, OPTIONS — these methods carry no request body.
    PyObject* json_body_obj = Py_None;
    if (!skip_body_parse && !req.body.empty()) {
        if (is_form_local && content_type_sv.len > 0) {
            // Check content-type to determine form type
            bool is_urlencoded = false, is_multipart = false;
            // Zero-allocation case-insensitive content-type check
            if (ci_contains(content_type_sv.data, content_type_sv.len, "application/x-www-form-urlencoded", 33)) {
                is_urlencoded = true;
            } else if (ci_contains(content_type_sv.data, content_type_sv.len, "multipart/form-data", 19)) {
                is_multipart = true;
            }

            if (is_urlencoded) {
                // Parse urlencoded body → list of (key, value) tuples → merge into kwargs
                const char* p = req.body.data;
                const char* end = p + req.body.len;
                PyRef form_dict(PyDict_New());
                // Scratch buffers — reused across iterations (same pattern as query params)
                std::string decoded_form_key, decoded_form_val;
                while (p < end) {
                    const char* key_start = p;
                    const char* eq = nullptr;
                    while (p < end && *p != '&') {
                        if (*p == '=' && !eq) eq = p;
                        p++;
                    }
                    if (eq) {
                        const char* raw_key = key_start;
                        size_t raw_key_len = eq - key_start;
                        const char* raw_val = eq + 1;
                        size_t raw_val_len = p - eq - 1;

                        // Single-pass percent-decode key and value (handles %XX and + → space)
                        std::string_view key_sv;
                        if (percent_decode_into_if_needed(decoded_form_key, raw_key, raw_key_len)) {
                            key_sv = std::string_view(decoded_form_key);
                        } else {
                            key_sv = std::string_view(raw_key, raw_key_len);
                        }
                        std::string_view val_sv;
                        if (percent_decode_into_if_needed(decoded_form_val, raw_val, raw_val_len)) {
                            val_sv = std::string_view(decoded_form_val);
                        } else {
                            val_sv = std::string_view(raw_val, raw_val_len);
                        }

                        PyRef pk(PyUnicode_FromStringAndSize(key_sv.data(), key_sv.size()));
                        PyRef pv(PyUnicode_FromStringAndSize(val_sv.data(), val_sv.size()));
                        if (pk && pv) {
                            // Handle multi-value fields: if key already exists, collect into list
                            PyObject* existing = PyDict_GetItem(form_dict.get(), pk.get());  // borrowed
                            if (existing == nullptr) {
                                // First occurrence — store as plain string
                                PyDict_SetItem(form_dict.get(), pk.get(), pv.get());
                                // Only inject raw values directly into kwargs for embedded
                                // (individual) params (embed_body_local=true). For single-model
                                // params (embed_body_local=false), kwargs gets the validated model
                                // from request_body_to_args, NOT raw field strings. Injecting raw
                                // values causes "unexpected keyword arguments" TypeError.
                                if (embed_body_local) {
                                    PyDict_SetItem(kwargs.get(), pk.get(), pv.get());
                                }
                            } else if (PyList_Check(existing)) {
                                // Already a list — append new value
                                PyList_Append(existing, pv.get());
                                if (embed_body_local) {
                                    PyDict_SetItem(kwargs.get(), pk.get(), existing);
                                }
                            } else {
                                // Second occurrence — promote to [existing, new] list
                                PyRef lst(PyList_New(0));
                                if (lst) {
                                    PyList_Append(lst.get(), existing);  // INCREF'd by Append
                                    PyList_Append(lst.get(), pv.get()); // INCREF'd by Append
                                    PyDict_SetItem(form_dict.get(), pk.get(), lst.get());
                                    if (embed_body_local) {
                                        PyDict_SetItem(kwargs.get(), pk.get(), lst.get());
                                    }
                                }
                            }
                        }
                    }
                    if (p < end) p++;
                }
                json_body_obj = form_dict.release();  // for Pydantic validation path
            } else if (is_multipart) {
                // Extract boundary from content-type (zero-alloc: scan raw string_view)
                const char* ct_ptr = content_type_sv.data;
                size_t ct_len = content_type_sv.len;
                size_t bpos = std::string_view(ct_ptr, ct_len).find("boundary=");
                if (bpos == std::string_view::npos) {
                    // Try case-insensitive
                    for (size_t i = 0; i + 9 <= ct_len; i++) {
                        if (ci_starts_with(ct_ptr + i, ct_len - i, "boundary=", 9)) {
                            bpos = i; break;
                        }
                    }
                }
                if (bpos != std::string_view::npos) {
                    bpos += 9;  // skip "boundary="
                    // Find end of boundary (';' or end of string)
                    size_t bend = bpos;
                    while (bend < ct_len && ct_ptr[bend] != ';') bend++;
                    // Use string_view into original content-type for boundary
                    const char* bnd_start = ct_ptr + bpos;
                    size_t bnd_len = bend - bpos;
                    // Strip quotes if present (using string_view — no alloc)
                    if (bnd_len >= 2 && bnd_start[0] == '"' && bnd_start[bnd_len - 1] == '"') {
                        bnd_start++;
                        bnd_len -= 2;
                    }

                    // Call C++ multipart parser via Python wrapper
                    PyRef body_bytes(PyBytes_FromStringAndSize(req.body.data, req.body.len));
                    PyRef boundary_str(PyUnicode_FromStringAndSize(bnd_start, bnd_len));
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
                                        // File upload — create UploadFile(filename, file=BytesIO(data), ...)
                                        // Lazy-init: get UploadFile class if not already cached
                                        if (!s_upload_file_class) {
                                            PyRef m(PyImport_ImportModule("astraapi._datastructures_impl"));
                                            if (m) {
                                                s_upload_file_class = PyObject_GetAttrString(m.get(), "UploadFile");
                                            } else { PyErr_Clear(); }
                                        }
                                        PyObject* upload_file_obj = nullptr;
                                        if (s_upload_file_class && data_obj) {
                                            // Build BytesIO(data) for the file-like backing
                                            PyRef io_mod(PyImport_ImportModule("io"));
                                            if (io_mod) {
                                                PyRef bytes_io_cls(PyObject_GetAttrString(io_mod.get(), "BytesIO"));
                                                if (bytes_io_cls) {
                                                    PyRef file_obj(PyObject_CallOneArg(bytes_io_cls.get(), data_obj));
                                                    if (file_obj) {
                                                        // Get content_type from part dict
                                                        PyObject* ct_obj = PyDict_GetItemString(part, "content_type");
                                                        const char* ct_str = "application/octet-stream";
                                                        if (ct_obj && ct_obj != Py_None && PyUnicode_Check(ct_obj)) {
                                                            const char* ct_tmp = PyUnicode_AsUTF8(ct_obj);
                                                            if (ct_tmp) ct_str = ct_tmp;
                                                        }
                                                        // UploadFile(filename=..., file=..., content_type=...)
                                                        PyRef kw(PyDict_New());
                                                        if (kw) {
                                                            if (!s_kw_filename) s_kw_filename = PyUnicode_InternFromString("filename");
                                                            if (!s_kw_file) s_kw_file = PyUnicode_InternFromString("file");
                                                            if (!s_kw_ct) s_kw_ct = PyUnicode_InternFromString("content_type");
                                                            PyRef ct_pystr(PyUnicode_FromString(ct_str));
                                                            if (ct_pystr) {
                                                                PyDict_SetItem(kw.get(), s_kw_filename, fn_obj);
                                                                PyDict_SetItem(kw.get(), s_kw_file, file_obj.get());
                                                                PyDict_SetItem(kw.get(), s_kw_ct, ct_pystr.get());
                                                                upload_file_obj = PyObject_Call(s_upload_file_class, g_empty_tuple, kw.get());
                                                                if (!upload_file_obj) PyErr_Clear();
                                                            }
                                                        }
                                                    }
                                                }
                                            } else { PyErr_Clear(); }
                                        }
                                        if (upload_file_obj) {
                                            // Handle multi-value: collect into list if key exists
                                            PyObject* existing_fd = PyDict_GetItem(form_dict.get(), name_obj);
                                            if (existing_fd == nullptr) {
                                                PyDict_SetItem(form_dict.get(), name_obj, upload_file_obj);
                                                if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, upload_file_obj);
                                            } else if (PyList_Check(existing_fd)) {
                                                PyList_Append(existing_fd, upload_file_obj);
                                                if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, existing_fd);
                                            } else {
                                                PyRef lst(PyList_New(0));
                                                if (lst) { PyList_Append(lst.get(), existing_fd); PyList_Append(lst.get(), upload_file_obj);
                                                    PyDict_SetItem(form_dict.get(), name_obj, lst.get());
                                                    if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, lst.get()); }
                                            }
                                            Py_DECREF(upload_file_obj);
                                        } else {
                                            PyDict_SetItem(form_dict.get(), name_obj, part);
                                        }
                                    } else if (data_obj) {
                                        // Simple form field
                                        PyRef str_val;
                                        if (PyBytes_Check(data_obj)) {
                                            char* d; Py_ssize_t dlen;
                                            PyBytes_AsStringAndSize(data_obj, &d, &dlen);
                                            str_val = PyRef(PyUnicode_FromStringAndSize(d, dlen));
                                        } else {
                                            str_val = PyRef(data_obj); Py_INCREF(data_obj);
                                        }
                                        if (str_val) {
                                            PyObject* existing_fd2 = PyDict_GetItem(form_dict.get(), name_obj);
                                            if (existing_fd2 == nullptr) {
                                                PyDict_SetItem(form_dict.get(), name_obj, str_val.get());
                                                if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, str_val.get());
                                            } else if (PyList_Check(existing_fd2)) {
                                                PyList_Append(existing_fd2, str_val.get());
                                                if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, existing_fd2);
                                            } else {
                                                PyRef lst(PyList_New(0));
                                                if (lst) { PyList_Append(lst.get(), existing_fd2); PyList_Append(lst.get(), str_val.get());
                                                    PyDict_SetItem(form_dict.get(), name_obj, lst.get());
                                                    if (embed_body_local) PyDict_SetItem(kwargs.get(), name_obj, lst.get()); }
                                            }
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
            // Validate content-type: must be application/json (or absent/empty)
            // Check content-type: allow application/json and application/*+json
            bool ct_is_json = false;
            if (content_type_sv.empty()) {
                ct_is_json = true;  // No content-type: assume JSON
            } else if (ci_contains(content_type_sv.data, content_type_sv.len, "application/json", 16)) {
                ct_is_json = true;
            } else {
                // Check for application/*+json (e.g. application/geo+json)
                // but NOT application/*+json-* (e.g. application/geo+json-seq)
                for (size_t ci = 0; ci + 5 <= content_type_sv.len; ci++) {
                    if (content_type_sv.data[ci] == '+' || content_type_sv.data[ci] == '+') {
                        if (ci + 5 <= content_type_sv.len) {
                            char lc[5]; for (int li=0;li<5;li++) lc[li]=tolower((unsigned char)content_type_sv.data[ci+li]);
                            if (memcmp(lc, "+json", 5) == 0) {
                                // Check what follows +json
                                size_t after = ci + 5;
                                // Skip to end or semicolon (params)
                                while (after < content_type_sv.len && content_type_sv.data[after] != ';' && content_type_sv.data[after] != ' ') after++;
                                // If nothing follows +json (or only whitespace/params), it is valid
                                if (after == ci + 5) { ct_is_json = true; }
                                break;
                            }
                        }
                    }
                }
            }
            if (!ct_is_json) {
                // Non-JSON content type for JSON body endpoint: return 422
                PyRef body_str(PyUnicode_FromStringAndSize(req.body.data, (Py_ssize_t)req.body.len));
                PyRef err_list(PyList_New(1));
                if (err_list && body_str) {
                    PyRef err_dict(PyDict_New());
                    if (err_dict) {
                        if (!s_ct_type_key) s_ct_type_key = PyUnicode_InternFromString("type");
                        if (!s_ct_loc_key) s_ct_loc_key = PyUnicode_InternFromString("loc");
                        if (!s_ct_msg_key) s_ct_msg_key = PyUnicode_InternFromString("msg");
                        if (!s_ct_input_key) s_ct_input_key = PyUnicode_InternFromString("input");
                        if (!s_ct_mat_val) s_ct_mat_val = PyUnicode_InternFromString("model_attributes_type");
                        if (!s_ct_mat_msg) s_ct_mat_msg = PyUnicode_InternFromString("Input should be a valid dictionary or object to extract fields from");
                        if (!s_ct_body_str) s_ct_body_str = PyUnicode_InternFromString("body");
                        PyRef loc(PyTuple_Pack(1, s_ct_body_str));
                        if (loc) {
                            PyDict_SetItem(err_dict.get(), s_ct_type_key, s_ct_mat_val);
                            PyDict_SetItem(err_dict.get(), s_ct_loc_key, loc.get());
                            PyDict_SetItem(err_dict.get(), s_ct_msg_key, s_ct_mat_msg);
                            PyDict_SetItem(err_dict.get(), s_ct_input_key, body_str.get());
                            PyList_SET_ITEM(err_list.get(), 0, err_dict.release());
                            PyRef outer(PyDict_New());
                            if (outer) {
                                if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                                PyDict_SetItem(outer.get(), s_detail_key_global, err_list.get());
                                PyRef err_json(serialize_to_json_pybytes(outer.get()));
                                if (err_json) {
                                    char* ej; Py_ssize_t ej_len;
                                    PyBytes_AsStringAndSize(err_json.get(), &ej, &ej_len);
                                    build_and_write_http_response(sock_fd, transport, 422, ej, (size_t)ej_len, req.keep_alive,
                                        has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                }
                            }
                        }
                    }
                }
                Py_DECREF(endpoint_local);
                Py_XDECREF(body_params_local);
                --self->counters.active_requests;
                ++self->counters.total_errors;
                return make_consumed_true(self, req.total_consumed);
            }
            yyjson_doc* doc = nullptr;
            const char* json_parse_err_msg = nullptr;
            PyObject* py_json_result = nullptr; // set if json.loads was used instead of yyjson
            // Check if json.loads is patched (e.g. in tests); if so, call it
            {
                static PyObject* _orig_json_loads = nullptr;
                static PyObject* _json_module_dict = nullptr;
                if (!_orig_json_loads) {
                    PyRef jm(PyImport_ImportModule("json"));
                    if (jm) {
                        _json_module_dict = PyModule_GetDict(jm.get());
                        Py_XINCREF(_json_module_dict);
                        _orig_json_loads = PyDict_GetItemString(_json_module_dict, "loads");
                        Py_XINCREF(_orig_json_loads);
                    }
                }
                PyObject* cur_loads_raw = _json_module_dict ? PyDict_GetItemString(_json_module_dict, "loads") : nullptr;
                if (cur_loads_raw && _orig_json_loads && cur_loads_raw != _orig_json_loads) {
                    // json.loads is patched -- call it
                    PyRef body_str(PyUnicode_DecodeUTF8(req.body.data, (Py_ssize_t)req.body.len, "replace"));
                    if (body_str) {
                        py_json_result = PyObject_CallOneArg(cur_loads_raw, body_str.get());
                        if (!py_json_result) {
                            // json.loads raised -- propagate as 400
                            goto handle_json_loads_exception;
                        }
                    }
                }
            }
            if (!py_json_result) {
            Py_BEGIN_ALLOW_THREADS
            doc = yyjson_parse_raw_with_err(req.body.data, req.body.len, &json_parse_err_msg);
            Py_END_ALLOW_THREADS
            }
            if (py_json_result) {
                // json.loads was patched and succeeded -- use its result
                if (spec.embed_body_fields && kwargs && PyDict_Check(py_json_result)) {
                    PyDict_Update(kwargs.get(), py_json_result);
                    json_body_obj = kwargs.release();
                    kwargs = PyRef(nullptr);
                } else {
                    json_body_obj = py_json_result;
                }
                py_json_result = nullptr; // ownership transferred
            } else if (doc) {
                if (spec.embed_body_fields && kwargs) {
                    // OPT: merge yyjson keys directly into kwargs in one pass,
                    // avoiding intermediate dict + PyDict_Update overhead
                    PyObject* full_dict = nullptr;
                    if (yyjson_doc_merge_to_dict(doc, kwargs.get(), &full_dict) == 0 && full_dict) {
                        json_body_obj = full_dict;  // for model_validate / InlineResult
                    }
                    // doc freed by yyjson_doc_merge_to_dict
                } else {
                    PyRef parsed(yyjson_doc_to_pyobject(doc));
                    if (parsed) {
                        json_body_obj = parsed.release();
                    } else {
                        PyErr_Clear();
                    }
                }
            } else if (req.body.len > 0) {
                // JSON parse failed on non-empty body: return 422 JSON decode error
                    --self->counters.active_requests;
                // Build error: [{"type":"json_invalid","loc":["body",1],"msg":"JSON decode error","input":{},"ctx":{"error":"..."}}]
                static const char json_err_prefix[] = "{\"detail\":[{\"type\":\"json_invalid\",\"loc\":[\"body\",1],\"msg\":\"JSON decode error\",\"input\":{},\"ctx\":{\"error\":\"";
                static const char json_err_suffix[] = "\"}}]}";
                std::string err_body = json_err_prefix;
                // Try Python json.loads to get the exact error message
                std::string py_err_msg;
                {
                    PyRef json_mod(PyImport_ImportModule("json"));
                    if (json_mod) {
                        PyRef loads_fn(PyObject_GetAttrString(json_mod.get(), "loads"));
                        if (loads_fn) {
                            PyRef body_str(PyUnicode_DecodeUTF8(req.body.data, (Py_ssize_t)req.body.len, "replace"));
                            if (body_str) {
                                PyRef result(PyObject_CallOneArg(loads_fn.get(), body_str.get()));
                                if (!result) {
                                    PyObject *et, *ev, *etb;
                                    PyErr_Fetch(&et, &ev, &etb);
                                    if (ev) {
                                        PyRef ev_str(PyObject_Str(ev));
                                        if (ev_str) {
                                            const char* s = PyUnicode_AsUTF8(ev_str.get());
                                            if (s) py_err_msg = s;
                                        }
                                    }
                                    Py_XDECREF(et); Py_XDECREF(ev); Py_XDECREF(etb);
                                }
                            }
                        }
                    }
                }
                // Strip position info from Python json error (e.g. ": line 1 column 2 (char 1)")
                if (!py_err_msg.empty()) {
                    auto colon_pos = py_err_msg.find(": line ");
                    if (colon_pos != std::string::npos) {
                        py_err_msg = py_err_msg.substr(0, colon_pos);
                    }
                }
                const char* err_msg = py_err_msg.empty() ? (json_parse_err_msg ? json_parse_err_msg : "Invalid JSON") : py_err_msg.c_str();
                // Escape the error message for JSON
                for (const char* p = err_msg; *p; ++p) {
                    if (*p == '"') err_body += "\\\"";
                    else if (*p == '\\') err_body += "\\\\";
                    else err_body += *p;
                }
                err_body += json_err_suffix;
                auto buf = acquire_buffer();
                buf.reserve(256);
                static const char hdr422[] = "HTTP/1.1 422 Unprocessable Entity\r\ncontent-type: application/json\r\ncontent-length: ";
                constexpr size_t hdr422_sz = sizeof(hdr422) - 1;
                std::string cl_str = std::to_string(err_body.size());
                buf.resize(hdr422_sz + cl_str.size() + 4 + err_body.size());
                size_t pos = 0;
                std::memcpy(buf.data() + pos, hdr422, hdr422_sz); pos += hdr422_sz;
                std::memcpy(buf.data() + pos, cl_str.data(), cl_str.size()); pos += cl_str.size();
                std::memcpy(buf.data() + pos, "\r\n\r\n", 4); pos += 4;
                std::memcpy(buf.data() + pos, err_body.data(), err_body.size());
                PyRef resp(PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size()));
                release_buffer(std::move(buf));
                if (resp) write_response_direct(sock_fd, transport, resp.get());
                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                Py_DECREF(endpoint_local);
                Py_XDECREF(body_params_local);
                return make_consumed_true(self, req.total_consumed);
            }

            // Label for when patched json.loads raises an exception
            if (false) {
                handle_json_loads_exception:
                // json.loads raised -- return 400 with the exception message
                    --self->counters.active_requests;
                PyObject *et400, *ev400, *etb400;
                PyErr_Fetch(&et400, &ev400, &etb400);
                std::string exc_msg;
                if (ev400) {
                    PyRef ev_s(PyObject_Str(ev400));
                    if (ev_s) { const char* s = PyUnicode_AsUTF8(ev_s.get()); if (s) exc_msg = s; }
                }
                Py_XDECREF(et400); Py_XDECREF(ev400); Py_XDECREF(etb400);
                static const char hdr400[] = "HTTP/1.1 400 Bad Request\r\ncontent-type: application/json\r\ncontent-length: ";
                std::string body400 = "{\"detail\":\"" + exc_msg + "\"}";
                std::string cl400 = std::to_string(body400.size());
                std::string resp400 = std::string(hdr400) + cl400 + "\r\n\r\n" + body400;
                PyRef r400(PyBytes_FromStringAndSize(resp400.data(), (Py_ssize_t)resp400.size()));
                if (r400) write_response_direct(sock_fd, transport, r400.get());
                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                Py_DECREF(endpoint_local);
                Py_XDECREF(body_params_local);
                return make_consumed_true(self, req.total_consumed);
            }

            if (json_body_obj != Py_None && spec.py_body_param_name) {
                if (!spec.embed_body_fields) {
                    // Non-embed: set full body as a named kwarg (zero-alloc: pre-interned key)
                    PyDict_SetItem(kwargs.get(), spec.py_body_param_name, json_body_obj);
                }
                // embed case already merged directly above
            }
        }
    }

    // For form routes with Pydantic validation: wrap the form dict in a FormData instance
    // so request_body_to_args calls _extract_form_body which fills in model defaults.
    // This ensures error "input" shows the defaults (matching standard AstraAPI behavior).
    if (is_form_local && has_body_params_local && s_form_data_class) {
        PyObject* src = (json_body_obj != Py_None) ? json_body_obj : nullptr;
        PyRef empty_dict;
        if (!src) {
            empty_dict = PyRef(PyDict_New());
            src = empty_dict.get();
        }
        PyRef form_data_inst(PyObject_CallOneArg(s_form_data_class, src));
        if (form_data_inst) {
            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
            json_body_obj = form_data_inst.release();
        }
    }

    // For dep routes with form body: inject form fields into kwargs so dep_solver can access them
    if (is_form_local && spec.has_dependencies && json_body_obj != Py_None && kwargs) {
        PyObject* form_src = json_body_obj;
        PyRef raw_dict_attr(PyObject_GetAttrString(form_src, "_dict"));
        if (!raw_dict_attr) { PyErr_Clear(); }
        PyObject* form_dict_to_merge = (raw_dict_attr && PyDict_Check(raw_dict_attr.get()))
            ? raw_dict_attr.get()
            : (PyDict_Check(form_src) ? form_src : nullptr);
        if (form_dict_to_merge) {
            PyObject *fk, *fv;
            Py_ssize_t fpos = 0;
            while (PyDict_Next(form_dict_to_merge, &fpos, &fk, &fv)) {
                if (!PyDict_Contains(kwargs.get(), fk)) {
                    PyDict_SetItem(kwargs.get(), fk, fv);
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
                // s_rh_key pre-cached at startup by py_init_cached_refs()
                PyDict_SetItem(kwargs.get(), s_rh_key, headers_list.get());
            }

            // s_m_key, s_p_key pre-cached at startup
            // Use cached method string if available (avoids per-request allocation)
            bool method_cached = false;
            PyObject* method_str = get_cached_method(req.method.data, req.method.len, method_cached);
            if (method_str) {
                PyDict_SetItem(kwargs.get(), s_m_key, method_str);
                if (!method_cached) Py_DECREF(method_str);
            }

            PyRef path_str(PyUnicode_FromStringAndSize(req.path.data, (Py_ssize_t)req.path.len));
            if (path_str) PyDict_SetItem(kwargs.get(), s_p_key, path_str.get());

            // Inject parsed Authorization header (scheme + credentials)
            // Python dep solver uses these for native HTTPBearer/HTTPBasic handling
            if (!authorization_sv.empty()) {
                size_t space_pos = 0;
                while (space_pos < authorization_sv.len && authorization_sv.data[space_pos] != ' ')
                    space_pos++;

                // s_as_key, s_ac_key pre-cached at startup

                PyRef scheme(PyUnicode_FromStringAndSize(authorization_sv.data, (Py_ssize_t)space_pos));
                size_t cred_start = space_pos < authorization_sv.len ? space_pos + 1 : space_pos;
                PyRef creds(PyUnicode_FromStringAndSize(
                    authorization_sv.data + cred_start,
                    (Py_ssize_t)(authorization_sv.len - cred_start)));
                if (scheme) PyDict_SetItem(kwargs.get(), s_as_key, scheme.get());
                if (creds) PyDict_SetItem(kwargs.get(), s_ac_key, creds.get());
            }
        }

        // Inject raw body bytes for dep_solver (needed for form deps like OAuth2PasswordRequestForm)
        if (req.body.len > 0) {
            if (!s_body_key2) s_body_key2 = PyUnicode_InternFromString("__body__");
            if (!PyDict_Contains(kwargs.get(), s_body_key2)) {
                PyRef body_bytes(PyBytes_FromStringAndSize(req.body.data, (Py_ssize_t)req.body.len));
                if (body_bytes) PyDict_SetItem(kwargs.get(), s_body_key2, body_bytes.get());
            }
        }
        if (content_type_sv.len > 0) {
            if (!s_ct_key2) s_ct_key2 = PyUnicode_InternFromString("__content_type__");
            if (!PyDict_Contains(kwargs.get(), s_ct_key2)) {
                PyRef ct_str(PyUnicode_FromStringAndSize(content_type_sv.data, (Py_ssize_t)content_type_sv.len));
                if (ct_str) PyDict_SetItem(kwargs.get(), s_ct_key2, ct_str.get());
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
                    // s_fut_blocking pre-cached at startup
                    // Reset _asyncio_future_blocking if dep_raw is a proper Future object.
                    // Some awaitables (like asyncio.sleep(0) in Python 3.12+) yield None —
                    // skip attribute setting for None to avoid AttributeError.
                    if (dep_raw && dep_raw != Py_None) {
                        PyObject_SetAttr(dep_raw, s_fut_blocking, Py_False);
                        PyErr_Clear();  // Ignore AttributeError if object doesn't support it
                    }

                    // NOTE: Do NOT decrement active_requests here — Python will call
                    // record_request_end() after async DI completion
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    Py_XDECREF(body_params_local);

                    if (!s_async_di_tag) s_async_di_tag = PyUnicode_InternFromString("async_di");
                    Py_INCREF(s_async_di_tag);

                    PyObject* ka = req.keep_alive ? Py_True : Py_False;
                    Py_INCREF(ka);
                    PyRef di_info(PyTuple_Pack(7, s_async_di_tag, dep_call_result.release(),
                                 dep_raw, endpoint_local, kwargs.release(),
                                 get_cached_status(status_code_local), ka));
                    Py_XDECREF(dep_raw);
                    return make_consumed_obj(self, req.total_consumed, di_info.release());
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
                            std::string exc_hdrs = extract_http_exc_headers(dep_ev);
                            Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                            PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error",
                                       req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                                       exc_hdrs.empty() ? nullptr : exc_hdrs.c_str(), exc_hdrs.size()));
                            if (resp) write_response_direct(sock_fd, transport, resp.get());
                        } else {
                            Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                            PyRef resp(build_http_error_response(500, "Internal Server Error",
                                       req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                            if (resp) write_response_direct(sock_fd, transport, resp.get());
                        }
                        fire_post_response_hook(self, req.method.data, req.method.len,
                                                req.path.data, req.path.len, hook_status, request_start_time);
                    }
                    --self->counters.active_requests;
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    Py_DECREF(endpoint_local);
                    Py_XDECREF(body_params_local);
                    return make_consumed_true(self, req.total_consumed);
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
                    // Sentinel so _response_shim skips re-running dep_solver
                    if (!s_deps_ran_key) s_deps_ran_key = PyUnicode_InternFromString("__deps_ran__");
                    PyDict_SetItem(kwargs.get(), s_deps_ran_key, Py_True);
                    // Extract bg_tasks (index 2) from dep result and inject into kwargs
                    if (PyTuple_GET_SIZE(dep_raw) > 2) {
                        PyObject* dep_bg = PyTuple_GET_ITEM(dep_raw, 2);
                        if (dep_bg && dep_bg != Py_None) {
                            if (!s_bg_key) s_bg_key = PyUnicode_InternFromString("__bg_tasks__");
                            PyDict_SetItem(kwargs.get(), s_bg_key, dep_bg);
                        }
                    }

                    // If there are validation errors, return 422 with full error detail
                    if (dep_errors && PyList_Check(dep_errors) && PyList_GET_SIZE(dep_errors) > 0) {
                        // Incref dep_errors before decref dep_raw: dep_errors is a borrowed
                        // reference into dep_raw tuple; decref-ing dep_raw first would free it.
                        Py_INCREF(dep_errors);
                        Py_DECREF(dep_raw);
                        --self->counters.active_requests;
                        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                        Py_DECREF(endpoint_local);
                        Py_XDECREF(body_params_local);
                        // Serialize actual errors: {"detail": [...]} like param_validator path
                        PyRef err_dict(PyDict_New());
                        if (err_dict) {
                            if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                            PyDict_SetItem(err_dict.get(), s_detail_key_global, dep_errors);
                            PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                            if (err_json) {
                                char* ej_data; Py_ssize_t ej_len;
                                PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                build_and_write_http_response(sock_fd, transport, 422, ej_data, (size_t)ej_len, req.keep_alive,
                                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                            } else {
                                PyErr_Clear();
                                PyRef resp(build_http_error_response(422, "Validation Error", req.keep_alive,
                                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                                if (resp) write_response_direct(sock_fd, transport, resp.get());
                            }
                        }
                        Py_DECREF(dep_errors);
                        return make_consumed_true(self, req.total_consumed);
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
                    std::string exc_hdrs = extract_http_exc_headers(dep_ev);
                    Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                    PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error",
                               req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                               exc_hdrs.empty() ? nullptr : exc_hdrs.c_str(), exc_hdrs.size()));
                    if (resp) write_response_direct(sock_fd, transport, resp.get());
                } else {
                    Py_XDECREF(dep_et); Py_XDECREF(dep_ev); Py_XDECREF(dep_tb);
                    PyRef resp(build_http_error_response(500, "Internal Server Error",
                               req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                    if (resp) write_response_direct(sock_fd, transport, resp.get());
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, hook_status, request_start_time);
            }
            --self->counters.active_requests;
            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
            Py_DECREF(endpoint_local);
            Py_XDECREF(body_params_local);
            return make_consumed_true(self, req.total_consumed);
        }
    }

    // ── Pydantic body validation — inline from C++ (no event loop round-trip) ──
    // request_body_to_args() is async def but NEVER actually awaits for JSON bodies
    // (only awaits for FormData). So PyIter_Send completes inline → PYGEN_RETURN.
    // This makes POST JSON follow the exact same zero-transition path as GET.
    if (has_body_params_local && body_params_local) {

        // ── FAST PATH: plain dict body (no Pydantic model) ──────────────
        // For routes like `body: dict = Body(...)`, skip validation entirely
        if (spec.body_is_plain_dict && json_body_obj != Py_None && spec.py_body_param_name) {
            PyDict_SetItem(kwargs.get(), spec.py_body_param_name, json_body_obj);
            Py_XDECREF(body_params_local);
            goto body_done;
        }

        // ── FAST PATH: single Pydantic model → call model_validate directly ──
        // Avoids going through request_body_to_args async wrapper
        if (spec.model_validate && json_body_obj != Py_None && spec.py_body_param_name) {
            // When embed_body_fields=True, json_body_obj is the full dict (e.g. {"item": {...}}).
            // Extract the nested value for this param before calling model_validate.
            PyObject* model_input = json_body_obj;
            if (spec.embed_body_fields && PyDict_Check(json_body_obj)) {
                PyObject* nested = PyDict_GetItem(json_body_obj, spec.py_body_param_name);
                if (nested) {
                    model_input = nested;
                } else {
                    // Key not found: return 422 'field required' for the body param
                    if (!s_loc_key_global) s_loc_key_global = PyUnicode_InternFromString("loc");
                    if (!s_body_str_global) s_body_str_global = PyUnicode_InternFromString("body");
                    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                    PyRef err_item(PyDict_New());
                    if (err_item) {
                        PyRef loc(PyTuple_Pack(2, s_body_str_global, spec.py_body_param_name));
                        PyRef type_str(PyUnicode_InternFromString("missing"));
                        PyRef msg_str(PyUnicode_InternFromString("Field required"));
                        if (loc) PyDict_SetItem(err_item.get(), s_loc_key_global, loc.get());
                        PyDict_SetItemString(err_item.get(), "type", type_str.get());
                        PyDict_SetItemString(err_item.get(), "msg", msg_str.get());
                        PyDict_SetItemString(err_item.get(), "input", Py_None);
                        PyRef err_list(PyList_New(1));
                        if (err_list) {
                            Py_INCREF(err_item.get());
                            PyList_SET_ITEM(err_list.get(), 0, err_item.get());
                            PyRef err_dict(PyDict_New());
                            if (err_dict) {
                                PyDict_SetItem(err_dict.get(), s_detail_key_global, err_list.get());
                                PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                                if (err_json) {
                                    char* ej_data; Py_ssize_t ej_len;
                                    PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                    build_and_write_http_response(sock_fd, transport, 422, ej_data, (size_t)ej_len, req.keep_alive,
                                               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                }
                            }
                        }
                    }
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    Py_XDECREF(body_params_local);
                    Py_DECREF(endpoint_local);
                    --self->counters.active_requests;
                    ++self->counters.total_errors;
                    return make_consumed_true(self, req.total_consumed);
                }
            }
            PyRef validated(PyObject_CallOneArg(spec.model_validate, model_input));
            if (validated) {
                PyDict_SetItem(kwargs.get(), spec.py_body_param_name, validated.get());
                Py_XDECREF(body_params_local);
                goto body_done;
            } else {
                // Validation error — build 422 from exception
                PyObject *exc_type, *exc_val, *exc_tb;
                PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
                if (exc_val) {
                    PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
                    // Try to get .errors(include_url=False) from Pydantic ValidationError
                    PyRef errors_method(PyObject_GetAttrString(exc_val, "errors"));
                    if (errors_method) {
                        // Call errors(include_url=False) to strip pydantic URL field
                        if (!s_include_url_key) s_include_url_key = PyUnicode_InternFromString("include_url");
                        PyRef err_kw(PyDict_New());
                        if (err_kw) PyDict_SetItem(err_kw.get(), s_include_url_key, Py_False);
                        PyRef error_list(err_kw ? PyObject_Call(errors_method.get(), g_empty_tuple, err_kw.get())
                                                : PyObject_CallNoArgs(errors_method.get()));
                        if (error_list && PyList_Check(error_list.get())) {
                            // Prepend 'body' (and param name if embedded) to each error's 'loc' tuple
                            if (!s_loc_key_global) s_loc_key_global = PyUnicode_InternFromString("loc");
                            if (!s_body_str_global) s_body_str_global = PyUnicode_InternFromString("body");
                            Py_ssize_t prefix_count = spec.embed_body_fields ? 2 : 1;
                            for (Py_ssize_t ei = 0; ei < PyList_GET_SIZE(error_list.get()); ei++) {
                                PyObject* err_item = PyList_GET_ITEM(error_list.get(), ei);
                                if (!PyDict_Check(err_item)) continue;
                                PyObject* loc_obj = PyDict_GetItem(err_item, s_loc_key_global);
                                if (loc_obj) {
                                    Py_ssize_t loc_len = PySequence_Length(loc_obj);
                                    PyRef new_loc(PyTuple_New(loc_len + prefix_count));
                                    if (new_loc) {
                                        Py_INCREF(s_body_str_global);
                                        PyTuple_SET_ITEM(new_loc.get(), 0, s_body_str_global);
                                        if (spec.embed_body_fields) {
                                            Py_INCREF(spec.py_body_param_name);
                                            PyTuple_SET_ITEM(new_loc.get(), 1, spec.py_body_param_name);
                                        }
                                        for (Py_ssize_t li = 0; li < loc_len; li++) {
                                            PyObject* litem = PySequence_GetItem(loc_obj, li);
                                            if (litem) PyTuple_SET_ITEM(new_loc.get(), li + prefix_count, litem);
                                        }
                                        PyDict_SetItem(err_item, s_loc_key_global, new_loc.get());
                                    }
                                }
                            }
                            // Check for custom RequestValidationError handler
                            if (self->type_exception_handlers && PyDict_Size(self->type_exception_handlers) > 0) {
                                PyObject* mv_handler = nullptr;
                                { PyObject *th_key, *th_val; Py_ssize_t th_pos = 0;
                                  while (PyDict_Next(self->type_exception_handlers, &th_pos, &th_key, &th_val)) {
                                      PyRef cn(PyObject_GetAttrString(th_key, "__name__"));
                                      if (cn && PyUnicode_Check(cn.get())) {
                                          const char* cns = PyUnicode_AsUTF8(cn.get());
                                          if (cns && strcmp(cns, "RequestValidationError") == 0) { mv_handler = th_val; break; }
                                      } else PyErr_Clear();
                                  }
                                }
                                if (mv_handler) {
                                    if (!s_mv_rve_cls) { PyRef em(PyImport_ImportModule("astraapi.exceptions")); if (em) s_mv_rve_cls = PyObject_GetAttrString(em.get(), "RequestValidationError"); }
                                    if (s_mv_rve_cls) {
                                        if (!s_mv_body_kw) s_mv_body_kw = PyUnicode_InternFromString("body");
                                        PyRef mv_kw(PyDict_New()); if (mv_kw) PyDict_SetItem(mv_kw.get(), s_mv_body_kw, json_body_obj);
                                        PyRef mv_args(PyTuple_Pack(1, error_list.get()));
                                        PyRef mv_rve(mv_args && mv_kw ? PyObject_Call(s_mv_rve_cls, mv_args.get(), mv_kw.get()) : nullptr);
                                        if (mv_rve) {
                                            PyRef mv_raw(PyObject_CallFunctionObjArgs(mv_handler, Py_None, mv_rve.get(), nullptr));
                                            PyRef mv_result;
                                            if (mv_raw && PyCoro_CheckExact(mv_raw.get())) {
                                                PyObject* cr = nullptr; PySendResult sr = PyIter_Send(mv_raw.get(), Py_None, &cr);
                                                if (sr == PYGEN_RETURN && cr) mv_result = PyRef(cr); else if (cr) Py_DECREF(cr);
                                                if (sr != PYGEN_RETURN) PyErr_Clear();
                                            } else if (mv_raw) mv_result = std::move(mv_raw);
                                            if (mv_result) {
                                                int hsc = 422; PyRef sc_a(PyObject_GetAttrString(mv_result.get(), "status_code"));
                                                if (sc_a) { hsc = (int)PyLong_AsLong(sc_a.get()); if (hsc == -1 && PyErr_Occurred()) { PyErr_Clear(); hsc = 422; } }
                                                PyRef body_b(PyObject_GetAttrString(mv_result.get(), "body"));
                                                if (body_b && PyBytes_Check(body_b.get())) {
                                                    char* hb; Py_ssize_t hbl; PyBytes_AsStringAndSize(body_b.get(), &hb, &hbl);
                                                    build_and_write_http_response(sock_fd, transport, hsc, hb, (size_t)hbl, req.keep_alive, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                                } else PyErr_Clear();
                                            } else PyErr_Clear();
                                        }
                                    }
                                    Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                                    Py_DECREF(endpoint_local); Py_DECREF(body_params_local);
                                    --self->counters.active_requests; ++self->counters.total_errors;
                                    return make_consumed_true(self, req.total_consumed);
                                }
                            }
                            PyRef err_dict(PyDict_New());
                            if (err_dict) {
                                if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                                PyDict_SetItem(err_dict.get(), s_detail_key_global, error_list.get());
                                PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                                if (err_json) {
                                    char* ej_data; Py_ssize_t ej_len;
                                    PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                    build_and_write_http_response(sock_fd, transport, 422, ej_data, (size_t)ej_len, req.keep_alive,
                                               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                }
                            }
                            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                            if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                            Py_DECREF(endpoint_local);
                            Py_DECREF(body_params_local);
                            --self->counters.active_requests;
                            ++self->counters.total_errors;
                            return make_consumed_true(self, req.total_consumed);
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
            PyRef deps_mod(PyImport_ImportModule("astraapi.dependencies.utils"));
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
                                // Validation error
                                // If custom RequestValidationError handler exists, raise it
                                if (self->type_exception_handlers && PyDict_Size(self->type_exception_handlers) > 0) {
                                    // Find handler for RequestValidationError by iterating
                                    PyObject* handler = nullptr;
                                    {
                                        PyObject *th_key, *th_val; Py_ssize_t th_pos = 0;
                                        while (PyDict_Next(self->type_exception_handlers, &th_pos, &th_key, &th_val)) {
                                            // Check if body_errors list items are instances of th_key
                                            // We check by class name for simplicity
                                            PyRef cls_name(PyObject_GetAttrString(th_key, "__name__"));
                                            if (cls_name && PyUnicode_Check(cls_name.get())) {
                                                const char* cn = PyUnicode_AsUTF8(cls_name.get());
                                                if (cn && (strcmp(cn, "RequestValidationError") == 0 ||
                                                           strcmp(cn, "ValidationException") == 0 ||
                                                           strcmp(cn, "Exception") == 0)) {
                                                    handler = th_val;
                                                    break;
                                                }
                                            } else { PyErr_Clear(); }
                                        }
                                    }
                                    if (handler) {
                                        // Create RequestValidationError to pass to handler
                                        if (!s_rve_cls2) {
                                            PyRef exc_mod(PyImport_ImportModule("astraapi.exceptions"));
                                            if (exc_mod) s_rve_cls2 = PyObject_GetAttrString(exc_mod.get(), "RequestValidationError");
                                        }
                                        if (s_rve_cls2) {
                                            // Call RequestValidationError(errors, body=json_body_obj)
                                            if (!s_rve_body_kw) s_rve_body_kw = PyUnicode_InternFromString("body");
                                            PyRef rve_kw(PyDict_New());
                                            if (rve_kw) PyDict_SetItem(rve_kw.get(), s_rve_body_kw, json_body_obj);
                                            PyRef rve_args(PyTuple_Pack(1, body_errors));
                                            PyRef rve(rve_args && rve_kw ? PyObject_Call(s_rve_cls2, rve_args.get(), rve_kw.get()) : nullptr);
                                            if (rve) {
                                                PyRef th_raw(PyObject_CallFunctionObjArgs(handler, Py_None, rve.get(), nullptr));
                                                // Drive coroutine if async handler
                                                PyRef th_result;
                                                if (th_raw && PyCoro_CheckExact(th_raw.get())) {
                                                    PyObject* coro_result = nullptr;
                                                    PySendResult sr = PyIter_Send(th_raw.get(), Py_None, &coro_result);
                                                    if (sr == PYGEN_RETURN && coro_result) th_result = PyRef(coro_result);
                                                    else if (coro_result) Py_DECREF(coro_result);
                                                    if (sr != PYGEN_RETURN) PyErr_Clear();
                                                } else if (th_raw) {
                                                    th_result = std::move(th_raw);
                                                }
                                                if (th_result) {
                                                    int hsc = 422;
                                                    PyRef sc_attr(PyObject_GetAttrString(th_result.get(), "status_code"));
                                                    if (sc_attr) { hsc = (int)PyLong_AsLong(sc_attr.get()); if (hsc == -1 && PyErr_Occurred()) { PyErr_Clear(); hsc = 422; } }
                                                    PyRef body_b(PyObject_GetAttrString(th_result.get(), "body"));
                                                    if (body_b && PyBytes_Check(body_b.get())) {
                                                        char* hbody; Py_ssize_t hbody_len;
                                                        PyBytes_AsStringAndSize(body_b.get(), &hbody, &hbody_len);
                                                        build_and_write_http_response(sock_fd, transport, hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                                            has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                                    } else { PyErr_Clear(); }
                                                } else { PyErr_Clear(); }
                                            }
                                        }
                                        Py_DECREF(validation_result);
                                        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                                        Py_DECREF(endpoint_local);
                                        Py_DECREF(body_params_local);
                                        --self->counters.active_requests;
                                        ++self->counters.total_errors;
                                        return make_consumed_true(self, req.total_consumed);
                                        } // if (s_rve_cls2)
                                    } // if (handler)
                                // Build {"detail": errors_list} and serialize as JSON
                                PyRef err_dict(PyDict_New());
                                if (err_dict) {
                                    if (!s_detail_key_global) s_detail_key_global = PyUnicode_InternFromString("detail");
                                    PyDict_SetItem(err_dict.get(), s_detail_key_global, body_errors);
                                    PyRef err_json(serialize_to_json_pybytes(err_dict.get()));
                                    if (err_json) {
                                        char* ej_data; Py_ssize_t ej_len;
                                        PyBytes_AsStringAndSize(err_json.get(), &ej_data, &ej_len);
                                        build_and_write_http_response(sock_fd, transport, 422, ej_data, (size_t)ej_len, req.keep_alive,
                                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                                    }
                                }
                                Py_DECREF(validation_result);
                                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                                Py_DECREF(endpoint_local);
                                Py_DECREF(body_params_local);
                                --self->counters.active_requests;
                                ++self->counters.total_errors;
                                return make_consumed_true(self, req.total_consumed);
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

            --self->counters.active_requests;
            return make_consumed_obj(self, req.total_consumed, (PyObject*)ir);
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
            // PyObject_Call is safe for all callable types (wrapped funcs, class instances, async gens).
            coro = PyRef(PyObject_Call(endpoint_local, g_empty_tuple, kwargs.get()));
        }
    }
    Py_DECREF(endpoint_local);  // release our strong ref
    if (!coro) {
        if (PyErr_Occurred()) {
            // ── Exception handler dispatch ──────────────────────────────
            PyObject *exc_type, *exc_val, *exc_tb;
            PyErr_Fetch(&exc_type, &exc_val, &exc_tb);
            // Check type-keyed exception handlers (user-registered, first match wins)
            if (self->type_exception_handlers && PyDict_Size(self->type_exception_handlers) > 0) {
                PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
                PyObject *th_key, *th_val;
                Py_ssize_t th_pos = 0;
                while (PyDict_Next(self->type_exception_handlers, &th_pos, &th_key, &th_val)) {
                    int is_inst = PyObject_IsInstance(exc_val, th_key);
                    if (is_inst > 0) {
                        PyRef th_result(PyObject_CallFunctionObjArgs(th_val, Py_None, exc_val, nullptr));
                        Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                        if (th_result) {
                            int hsc = 200;
                            PyRef sc_attr(PyObject_GetAttrString(th_result.get(), "status_code"));
                            if (sc_attr) { hsc = (int)PyLong_AsLong(sc_attr.get()); if (hsc == -1 && PyErr_Occurred()) { PyErr_Clear(); hsc = 200; } }
                            PyRef body_b(PyObject_GetAttrString(th_result.get(), "body"));
                            if (body_b && PyBytes_Check(body_b.get())) {
                                char* hbody; Py_ssize_t hbody_len;
                                PyBytes_AsStringAndSize(body_b.get(), &hbody, &hbody_len);
                                build_and_write_http_response(sock_fd, transport, hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                            } else { PyErr_Clear(); }
                        } else { PyErr_Clear(); }
                        fire_post_response_hook(self, req.method.data, req.method.len,
                                                req.path.data, req.path.len, 200, request_start_time);
                        --self->counters.active_requests;
                        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                        return make_consumed_true(self, req.total_consumed);
                    } else if (is_inst < 0) { PyErr_Clear(); }
                }
            }
            if (is_http_exception(exc_type)) {
                PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
                PyRef detail(PyObject_GetAttrString(exc_val, "detail"));
                PyRef sc(PyObject_GetAttrString(exc_val, "status_code"));
                int scode = sc ? (int)PyLong_AsLong(sc.get()) : 500;
                if (scode == -1 && PyErr_Occurred()) { PyErr_Clear(); scode = 500; }
                std::string exc_hdrs_main = extract_http_exc_headers(exc_val);

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
                            build_and_write_http_response(sock_fd, transport, hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                       has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                        }
                    } else {
                        // Handler raised an exception — clear it before falling through to 500
                        PyErr_Clear();
                    }
                    fire_post_response_hook(self, req.method.data, req.method.len,
                                            req.path.data, req.path.len, scode, request_start_time);
                    --self->counters.active_requests;
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    return make_consumed_true(self, req.total_consumed);
                }

                Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
                exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
                if (detail && sc) {
                    PyRef detail_str(PyObject_Str(detail.get()));
                    const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                    PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error", req.keep_alive,
                               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                               exc_hdrs_main.empty() ? nullptr : exc_hdrs_main.c_str(), exc_hdrs_main.size()));
                    if (resp) write_response_direct(sock_fd, transport, resp.get());
                    fire_post_response_hook(self, req.method.data, req.method.len,
                                            req.path.data, req.path.len, scode, request_start_time);
                    --self->counters.active_requests;
                    if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                    return make_consumed_true(self, req.total_consumed);
                }
            }
            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
        }
        // 500 error
        PyRef resp(build_http_error_response(500, "Internal Server Error", req.keep_alive,
                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
        if (resp) write_response_direct(sock_fd, transport, resp.get());
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, 500, request_start_time);
        --self->counters.active_requests;
        ++self->counters.total_errors;
        if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
        return make_consumed_true(self, req.total_consumed);
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
            // s_validate_str pre-cached at startup; s_serialize stays lazy (rare path)
            if (!s_serialize) s_serialize = PyUnicode_InternFromString("serialize_python");

            // field.validate_python(raw_result) → validated model
            PyRef validated(PyObject_CallMethodOneArg(response_model_local, s_validate_str, raw_result));
            if (!validated) {
                // Validation error → 422
                PyErr_Clear();
                Py_DECREF(raw_result);
                PyRef resp(build_http_error_response(422, "Response validation error", req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                if (resp) write_response_direct(sock_fd, transport, resp.get());
                --self->counters.active_requests;
                return make_consumed_true(self, req.total_consumed);
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
        // If HTTP middleware is registered, return result to Python for middleware processing
        if (self->has_http_middleware && raw_result != nullptr) {
            PyObject* ka = req.keep_alive ? Py_True : Py_False;
            Py_INCREF(ka);
            if (!s_mw_tag) s_mw_tag = PyUnicode_InternFromString("mw");
            Py_INCREF(s_mw_tag);
            // Build headers list for middleware Request
            PyRef hdrs_list(PyList_New(req.header_count));
            if (hdrs_list) {
                for (int i = 0; i < req.header_count; i++) {
                    const auto& hdr = req.headers[i];
                    PyRef nb(PyBytes_FromStringAndSize(hdr.name.data, (Py_ssize_t)hdr.name.len));
                    PyRef vb(PyBytes_FromStringAndSize(hdr.value.data, (Py_ssize_t)hdr.value.len));
                    if (nb && vb) { PyRef p(PyTuple_Pack(2, nb.get(), vb.get())); if (p) PyList_SET_ITEM(hdrs_list.get(), i, p.release()); else { Py_INCREF(Py_None); PyList_SET_ITEM(hdrs_list.get(), i, Py_None); } }
                    else { Py_INCREF(Py_None); PyList_SET_ITEM(hdrs_list.get(), i, Py_None); }
                }
            }
            bool mc = false;
            PyObject* method_str = get_cached_method(req.method.data, req.method.len, mc);
            PyRef path_str(PyUnicode_FromStringAndSize(req.path.data, (Py_ssize_t)req.path.len));
            PyRef qs_bytes(PyBytes_FromStringAndSize(req.query_string.data, (Py_ssize_t)req.query_string.len));
            PyRef mw_info(PyTuple_Pack(7, s_mw_tag, raw_result, get_cached_status(status_code_local), ka,
                hdrs_list ? hdrs_list.get() : Py_None,
                method_str ? method_str : Py_None,
                path_str ? path_str.get() : Py_None));
            if (!mc && method_str) Py_DECREF(method_str);
            Py_DECREF(ka);
            Py_DECREF(raw_result);
            if (mw_info) {
                if (json_body_obj != Py_None) Py_DECREF(json_body_obj);
                Py_XDECREF(body_params_local);
                return make_consumed_obj(self, req.total_consumed, mw_info.release());
            }
        }
        if (LIKELY(PyDict_Check(raw_result) || PyList_Check(raw_result) ||
            PyUnicode_Check(raw_result) || PyLong_Check(raw_result) ||
            PyFloat_Check(raw_result) || PyBool_Check(raw_result) ||
            PyTuple_Check(raw_result) || PySet_Check(raw_result) ||
            PyFrozenSet_Check(raw_result) || raw_result == Py_None)) {

            // Fused: serialize JSON + build HTTP + write — zero intermediate PyBytes
            int wrc = serialize_json_and_write_response(
                sock_fd, transport, raw_result, status_code_local, req.keep_alive,
                accept_encoding_sv.data, accept_encoding_sv.len,
                has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                is_head_method);
            Py_DECREF(raw_result);
            if (wrc < 0) {
                --self->counters.active_requests;
                ++self->counters.total_errors;
                return make_consumed_true(self, req.total_consumed);
            }

            fire_post_response_hook(self, req.method.data, req.method.len,
                                    req.path.data, req.path.len, status_code_local, request_start_time);
            --self->counters.active_requests;
            return make_consumed_true(self, req.total_consumed);
        }

        // ── Response object detection ─────────────────────────────────────
        // Check if result is a Starlette Response (has .body, .status_code, .raw_headers)
        // Use HasAttrString first to avoid expensive exception set+clear on dict/list

        // Special case: StreamingResponse (has body_iterator) — dispatch to Python's
        // _handle_stream for proper async iteration without blocking the event loop.
        // Python will call record_request_end() when done, so do NOT decrement here.
        if (s_attr_body_iterator && PyObject_HasAttr(raw_result, s_attr_body_iterator)) {
            PyObject* ka = req.keep_alive ? Py_True : Py_False;
            PyRef stream_info(PyTuple_Pack(4, s_stream_tag, raw_result,
                                           get_cached_status(status_code_local), ka));
            if (stream_info) {
                Py_DECREF(raw_result);  // PyTuple_Pack holds its own ref; release ours
                return make_consumed_obj(self, req.total_consumed, stream_info.release());
            }
            // fallthrough on pack failure
        }

        // Special case: FileResponse (has path, body is empty placeholder) — dispatch to Python
        if (s_attr_path && s_attr_body_iterator &&
            PyObject_HasAttr(raw_result, s_attr_path) &&
            !PyObject_HasAttr(raw_result, s_attr_body_iterator)) {
            PyRef path_attr(PyObject_GetAttr(raw_result, s_attr_path));
            if (path_attr && PyUnicode_Check(path_attr.get())) {
                PyObject* ka = req.keep_alive ? Py_True : Py_False;
                PyRef stream_info(PyTuple_Pack(4, s_stream_tag, raw_result,
                                               get_cached_status(status_code_local), ka));
                if (stream_info) {
                    Py_DECREF(raw_result);
                    return make_consumed_obj(self, req.total_consumed, stream_info.release());
                }
            }
        }

        PyRef body_attr, sc_attr;
        if (s_attr_body && s_attr_status_code &&
            PyObject_HasAttr(raw_result, s_attr_body) &&
            PyObject_HasAttr(raw_result, s_attr_status_code)) {
            body_attr = PyRef(PyObject_GetAttr(raw_result, s_attr_body));
            sc_attr = PyRef(PyObject_GetAttr(raw_result, s_attr_status_code));
        }

        if (body_attr && PyBytes_Check(body_attr.get()) && sc_attr && PyLong_Check(sc_attr.get())) {
            // This is a Response object — extract body + status + headers
            int resp_sc = (int)PyLong_AsLong(sc_attr.get());
            char* resp_body; Py_ssize_t resp_body_len;
            PyBytes_AsStringAndSize(body_attr.get(), &resp_body, &resp_body_len);

            // Extract raw_headers to build custom header block
            // Local astraapi Response uses _raw_headers (list), while Starlette
            // uses raw_headers (also list). Try _raw_headers first (faster), fall back.
            PyRef raw_hdrs;
            if (s_attr_raw_headers && PyObject_HasAttr(raw_result, s_attr_raw_headers)) {
                raw_hdrs = PyRef(PyObject_GetAttr(raw_result, s_attr_raw_headers));
            } else if (s_attr_raw_headers2) {
                raw_hdrs = PyRef(PyObject_GetAttr(raw_result, s_attr_raw_headers2));
            } else {
                raw_hdrs = PyRef(PyObject_GetAttrString(raw_result, "raw_headers"));
            }
            auto buf = acquire_buffer();
            buf.reserve(256 + (size_t)resp_body_len);

            // Build HTTP response status line (use cache if available)
            if (resp_sc > 0 && resp_sc < 600 && s_status_lines[resp_sc].data) {
                const auto& sl = s_status_lines[resp_sc];
                buf_append(buf, sl.data, sl.len - 2);  // exclude trailing \r\n
            } else {
                static const char prefix[] = "HTTP/1.1 ";
                buf_append(buf, prefix, sizeof(prefix) - 1);
                char sc_buf[8];
                int sn = fast_i64_to_buf(sc_buf, resp_sc);
                buf_append(buf, sc_buf, sn);
                buf.push_back(' ');
                const char* reason = status_reason(resp_sc);
                size_t rlen = strlen(reason);
                buf_append(buf, reason, rlen);
            }

            // Emit response headers from raw_headers list
            bool saw_content_length = false;
            if (raw_hdrs && PyList_Check(raw_hdrs.get())) {
                Py_ssize_t nhdr = PyList_GET_SIZE(raw_hdrs.get());
                for (Py_ssize_t hi = 0; hi < nhdr; hi++) {
                    PyObject* htuple = PyList_GET_ITEM(raw_hdrs.get(), hi);
                    if (PyTuple_Check(htuple) && PyTuple_GET_SIZE(htuple) >= 2) {
                        PyObject* hname = PyTuple_GET_ITEM(htuple, 0);
                        PyObject* hval = PyTuple_GET_ITEM(htuple, 1);
                        if (PyBytes_Check(hname) && PyBytes_Check(hval)) {
                            buf_append(buf, "\r\n", 2);
                            buf_append(buf, PyBytes_AS_STRING(hname), (size_t)PyBytes_GET_SIZE(hname));
                            buf_append(buf, ": ", 2);
                            buf_append(buf, PyBytes_AS_STRING(hval), (size_t)PyBytes_GET_SIZE(hval));
                            if (!saw_content_length) {
                                // Check if this is the content-length header
                                Py_ssize_t hn_len = PyBytes_GET_SIZE(hname);
                                const char* hn_str = PyBytes_AS_STRING(hname);
                                if (hn_len == 14 &&
                                    (hn_str[0] == 'c' || hn_str[0] == 'C') &&
                                    strncasecmp(hn_str, "content-length", 14) == 0) {
                                    saw_content_length = true;
                                }
                            }
                        }
                    }
                }
            }
            // Ensure content-length is present (needed for keep-alive HTTP/1.1)
            if (!saw_content_length) {
                char cl_buf[32];
                int cl_len = snprintf(cl_buf, sizeof(cl_buf), "\r\ncontent-length: %zu", (size_t)resp_body_len);
                buf_append(buf, cl_buf, (size_t)cl_len);
            }

            // CORS headers
            if (has_cors && !origin_sv.empty()) {
                build_cors_headers(buf, cors_ptr, origin_sv.data, origin_sv.len);
            }

            // Connection + end of headers
            static const char CONN_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
            static const char CONN_CLOSE[] = "\r\nconnection: close\r\n\r\n";
            if (req.keep_alive) {
                buf_append(buf, CONN_KA, sizeof(CONN_KA) - 1);
            } else {
                buf_append(buf, CONN_CLOSE, sizeof(CONN_CLOSE) - 1);
            }

            // Body
            buf_append(buf, resp_body, resp_body_len);

            PyRef http_resp(PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size()));
            release_buffer(std::move(buf));
            if (http_resp) write_response_direct(sock_fd, transport, http_resp.get());

            // ── Background tasks ─────────────────────────────────────────
            PyRef bg_attr(s_attr_background
                ? PyObject_GetAttr(raw_result, s_attr_background)
                : PyObject_GetAttrString(raw_result, "background"));
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
            --self->counters.active_requests;
            return make_consumed_true(self, req.total_consumed);
        }

        // ── Pydantic model: no response_model_field configured ──────────────────
        // model_dump_json(by_alias=True) returns JSON bytes directly — zero re-encode overhead.
        // Fixes endpoints that return Pydantic models without response_model_field.
        // by_alias=True matches AstraAPI's jsonable_encoder default behavior.
        {
            if (!s_mdj) s_mdj = PyUnicode_InternFromString("model_dump_json");
            if (!s_by_alias_kw) s_by_alias_kw = PyUnicode_InternFromString("by_alias");
            if (PyObject_HasAttr(raw_result, s_mdj)) {
                PyRef mdj_method(PyObject_GetAttr(raw_result, s_mdj));
                Py_DECREF(raw_result);
                raw_result = nullptr;
                if (mdj_method) {
                    // Call model_dump_json(by_alias=True) to match AstraAPI's jsonable_encoder
                    PyRef kw(PyDict_New());
                    if (kw) PyDict_SetItem(kw.get(), s_by_alias_kw, Py_True);
                    PyRef json_bytes(PyObject_Call(mdj_method.get(), g_empty_tuple, kw.get()));
                    if (json_bytes) {
                        const char* body_ptr = nullptr;
                        Py_ssize_t body_sz = 0;
                        if (PyBytes_Check(json_bytes.get())) {
                            body_ptr = PyBytes_AS_STRING(json_bytes.get());
                            body_sz  = PyBytes_GET_SIZE(json_bytes.get());
                        } else if (PyUnicode_Check(json_bytes.get())) {
                            body_ptr = PyUnicode_AsUTF8AndSize(json_bytes.get(), &body_sz);
                        }
                        if (body_ptr) {
                            build_and_write_http_response(
                                sock_fd, transport, status_code_local,
                                body_ptr, (size_t)body_sz, req.keep_alive,
                                has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                                nullptr, is_head_method);
                        }
                    } else { PyErr_Clear(); }
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, status_code_local, request_start_time);
                --self->counters.active_requests;
                return make_consumed_true(self, req.total_consumed);
            }
        }

        // ── Fallback: serialize as string ───────────────────────────────
        // Try dataclass/Pydantic model: call dataclasses.asdict or model_dump
        {
            if (!s_asdict) {
                PyRef dc_mod(PyImport_ImportModule("dataclasses"));
                if (dc_mod) s_asdict = PyObject_GetAttrString(dc_mod.get(), "asdict");
            }
            if (!s_is_dc) {
                PyRef dc_mod(PyImport_ImportModule("dataclasses"));
                if (dc_mod) s_is_dc = PyObject_GetAttrString(dc_mod.get(), "is_dataclass");
            }
            if (s_is_dc && s_asdict) {
                PyRef is_dc_result(PyObject_CallOneArg(s_is_dc, raw_result));
                if (is_dc_result && PyObject_IsTrue(is_dc_result.get())) {
                    PyRef dc_dict(PyObject_CallOneArg(s_asdict, raw_result));
                    if (dc_dict && PyDict_Check(dc_dict.get())) {
                        Py_DECREF(raw_result);
                        raw_result = dc_dict.release();
                        PyRef err_json(serialize_to_json_pybytes(raw_result));
                        Py_DECREF(raw_result);
                        if (err_json) {
                            char* ej; Py_ssize_t ejl;
                            PyBytes_AsStringAndSize(err_json.get(), &ej, &ejl);
                            build_and_write_http_response(sock_fd, transport, status_code_local, ej, (size_t)ejl, req.keep_alive,
                                has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                        }
                        fire_post_response_hook(self, req.method.data, req.method.len, req.path.data, req.path.len, status_code_local, request_start_time);
                        --self->counters.active_requests;
                        return make_consumed_true(self, req.total_consumed);
                    }
                } else if (!is_dc_result) PyErr_Clear();
            }
        }
        PyRef str_repr(PyObject_Str(raw_result));
        Py_DECREF(raw_result);
        if (str_repr) {
            Py_ssize_t slen;
            const char* s = PyUnicode_AsUTF8AndSize(str_repr.get(), &slen);
            if (s) {
                build_and_write_http_response(sock_fd, transport, status_code_local, s, (size_t)slen, req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
            }
        }
        fire_post_response_hook(self, req.method.data, req.method.len,
                                req.path.data, req.path.len, status_code_local, request_start_time);
        --self->counters.active_requests;
        return make_consumed_true(self, req.total_consumed);
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
        // s_fut_blocking pre-cached at startup
        if (raw_result) {
            PyObject_SetAttr(raw_result, s_fut_blocking, Py_False);
            PyErr_Clear();  // Clear AttributeError if raw_result is None or non-Future
        }

        // s_async_tag pre-cached at startup
        Py_INCREF(s_async_tag);

        PyObject* ka = req.keep_alive ? Py_True : Py_False;
        Py_INCREF(ka);
        PyRef async_info(PyTuple_Pack(5, s_async_tag, coro.release(),
                         raw_result, get_cached_status(status_code_local), ka));
        Py_XDECREF(raw_result);
        return make_consumed_obj(self, req.total_consumed, async_info.release());
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
            std::string exc_hdrs_async = extract_http_exc_headers(exc_val);

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
                        build_and_write_http_response(sock_fd, transport, hsc, hbody, (size_t)hbody_len, req.keep_alive,
                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                    }
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, scode, request_start_time);
                --self->counters.active_requests;
                return make_consumed_true(self, req.total_consumed);
            }

            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
            exc_type = exc_val = exc_tb = nullptr;  // prevent double-free
            if (detail && sc) {
                PyRef detail_str(PyObject_Str(detail.get()));
                const char* detail_cstr = detail_str ? PyUnicode_AsUTF8(detail_str.get()) : "Error";
                PyRef resp(build_http_error_response(scode, detail_cstr ? detail_cstr : "Error", req.keep_alive,
                           has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len,
                           exc_hdrs_async.empty() ? nullptr : exc_hdrs_async.c_str(), exc_hdrs_async.size()));
                if (resp) write_response_direct(sock_fd, transport, resp.get());
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, scode, request_start_time);
                --self->counters.active_requests;
                return make_consumed_true(self, req.total_consumed);
            }
        } else if (is_validation_exception(exc_type)) {
            PyErr_NormalizeException(&exc_type, &exc_val, &exc_tb);
            if (!s_errors_str2) s_errors_str2 = PyUnicode_InternFromString("errors");
            PyRef em(exc_val ? PyObject_GetAttr(exc_val, s_errors_str2) : nullptr);
            PyRef el_raw(em ? PyObject_CallNoArgs(em.get()) : nullptr);
            if (!el_raw) PyErr_Clear();
            // Strip "url" key from each error dict (pydantic v2 adds it)
            PyRef el(nullptr);
            if (el_raw && PyList_Check(el_raw.get())) {
                if (!s_url_key) s_url_key = PyUnicode_InternFromString("url");
                Py_ssize_t nerr = PyList_GET_SIZE(el_raw.get());
                PyRef filtered(PyList_New(nerr));
                if (filtered) {
                    for (Py_ssize_t ei = 0; ei < nerr; ei++) {
                        PyObject* orig = PyList_GET_ITEM(el_raw.get(), ei);
                        if (PyDict_Check(orig)) {
                            PyRef copy(PyDict_Copy(orig));
                            if (copy) { PyDict_DelItem(copy.get(), s_url_key); PyErr_Clear(); }
                            Py_INCREF(copy ? copy.get() : orig);
                            PyList_SET_ITEM(filtered.get(), ei, copy ? copy.release() : (Py_INCREF(orig), orig));
                        } else {
                            Py_INCREF(orig);
                            PyList_SET_ITEM(filtered.get(), ei, orig);
                        }
                    }
                    el = std::move(filtered);
                }
            } else {
                el = std::move(el_raw);
            }
            Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
            if (!s_det_key) s_det_key = PyUnicode_InternFromString("detail");
            PyRef dd(PyDict_New());
            if (dd && el) PyDict_SetItem(dd.get(), s_det_key, el.get());
            if (dd) {
                int wrc422 = serialize_json_and_write_response(
                    sock_fd, transport, dd.get(), 422, req.keep_alive,
                    nullptr, 0, has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len);
                if (wrc422 < 0) {
                    PyRef r422(build_http_error_response(422, "Validation Error", req.keep_alive,
                                   has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
                    if (r422) write_response_direct(sock_fd, transport, r422.get());
                }
                fire_post_response_hook(self, req.method.data, req.method.len,
                                        req.path.data, req.path.len, 422, request_start_time);
                --self->counters.active_requests;
                return make_consumed_true(self, req.total_consumed);
            }
        }
        Py_XDECREF(exc_type); Py_XDECREF(exc_val); Py_XDECREF(exc_tb);
    }


    PyRef resp(build_http_error_response(500, "Internal Server Error", req.keep_alive,
               has_cors ? cors_ptr : nullptr, origin_sv.data, origin_sv.len));
    if (resp) write_response_direct(sock_fd, transport, resp.get());
    fire_post_response_hook(self, req.method.data, req.method.len,
                            req.path.data, req.path.len, 500, request_start_time);
    --self->counters.active_requests;
    ++self->counters.total_errors;
    return make_consumed_true(self, req.total_consumed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// handle_http_batch — Batch HTTP dispatch (METH_FASTCALL)
//
// Processes ALL complete requests in the buffer in a single C++ call.
// For N pipelined/batched requests: N Python→C++ boundary crossings → 1.
// Eliminates N PyLong allocations (offset/last_consumed) per TCP segment.
//
// Returns:
//   True  = all sync requests processed, last_consumed = total bytes consumed
//   None  = need more data (nothing consumed)
//   False = parse error (400 already sent)
//   tuple = async/WS/Pydantic endpoint hit; tuple[0] = total consumed so far
// ═══════════════════════════════════════════════════════════════════════════════
static PyObject* CoreApp_handle_http_batch(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs < 2 || nargs > 4) {
        PyErr_SetString(PyExc_TypeError, "handle_http_batch requires 2-4 args");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];
    PyObject* transport  = args[1];
    Py_ssize_t base_offset = 0;
    if (nargs >= 3) {
        base_offset = PyLong_AsSsize_t(args[2]);
        if (base_offset < 0 && PyErr_Occurred()) return nullptr;
    }
    int sock_fd = -1;
    if (nargs >= 4) {
        sock_fd = (int)PyLong_AsLong(args[3]);
        if (sock_fd == -1 && PyErr_Occurred()) { PyErr_Clear(); sock_fd = -1; }
    }

    // Re-arm TCP_QUICKACK once for the whole batch (one syscall per data_received).
    if (sock_fd >= 0) platform_rearm_quickack(sock_fd);

    // Buffer access — same dual-path as handle_http.
    const char* raw_base;
    Py_ssize_t  raw_len;
    Py_buffer   view = {};
    bool        have_view = false;

    if (PyCapsule_CheckExact(buffer_obj)) {
        auto* hb = static_cast<HttpConnectionBuffer*>(
            PyCapsule_GetPointer(buffer_obj, HTTP_BUF_CAPSULE_NAME));
        if (!hb) return nullptr;
        Py_ssize_t total = (Py_ssize_t)hb->size();
        if (base_offset > total) base_offset = total;
        raw_base = (const char*)hb->data() + base_offset;
        raw_len  = total - base_offset;
    } else {
        if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) return nullptr;
        have_view = true;
        if (base_offset > view.len) base_offset = view.len;
        raw_base = (const char*)view.buf + base_offset;
        raw_len  = view.len - base_offset;
    }
    struct BufferGuard {
        Py_buffer* v; bool active;
        ~BufferGuard() { if (active) PyBuffer_Release(v); }
    } buf_guard{&view, have_view};

    // ── Batch loop: process all complete requests in C++ ────────────────────────
    // Only exits to Python for: async/WS/Pydantic endpoints, incomplete data,
    // or parse errors.  All sync requests are handled inline with zero Python cost.
    Py_ssize_t prefix = 0;  // bytes consumed by sync requests so far

    while (true) {
        PyObject* r = dispatch_one_request(
            self, raw_base + prefix, raw_len - prefix, transport, sock_fd);

        if (r == Py_True) {
            // Sync request completed — accumulate and loop.
            prefix += self->last_consumed;
            // Py_True is immortal in 3.12+; DECREF is safe on all versions.
            Py_DECREF(r);
            continue;
        }

        if (r == Py_None) {
            // Buffer exhausted (no more complete requests).
            Py_DECREF(r);
            if (prefix > 0) {
                // We processed some requests before stalling — report total.
                self->last_consumed = prefix;
                Py_RETURN_TRUE;
            }
            Py_RETURN_NONE;  // Genuinely nothing consumed — need more data.
        }

        if (r == Py_False) {
            // Parse error — 400 already sent, transport will be closed.
            // Report how many bytes were successfully consumed before the error.
            self->last_consumed = prefix;
            return r;
        }

        // Async / WS / Pydantic tuple — adjust tuple[0] to include the sync prefix
        // so Python's `offset += consumed` accounts for all processed requests.
        if (prefix > 0 && LIKELY(PyTuple_Check(r) && PyTuple_GET_SIZE(r) == 2)) {
            Py_ssize_t req_consumed = self->last_consumed; // set by make_consumed_obj
            Py_ssize_t total_c = prefix + req_consumed;
            PyObject* new_c = PyLong_FromSsize_t(total_c);
            if (LIKELY(new_c)) {
                Py_DECREF(PyTuple_GET_ITEM(r, 0));
                PyTuple_SET_ITEM(r, 0, new_c);
                self->last_consumed = total_c;
            }
        }
        return r;  // Python handles async work, then loops back with new offset.
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// handle_http_batch_v2 — Zero-overhead batch HTTP dispatch (METH_FASTCALL)
//
// Upgrade over handle_http_batch: auto-consumes the HttpConnectionBuffer
// internally, eliminating 3 Python→C++ calls per batch from the hot path:
//   _http_buf_consume  - no longer needed
//   _http_buf_len      - no longer needed
//   core.last_consumed - no longer needed (returned directly)
//
// Also strips the outer (consumed, inner) tuple wrapper for async results,
// eliminating 1 PyLong_FromSsize_t allocation + tuple modification per
// async request when sync requests precede it.
//
// Returns:
//   int  ≥ 0 = sync batch done; 0 = buffer empty, 1 = partial request pending
//   -1       = need more data (nothing consumed, buffer unchanged)
//   -2       = parse error (400 sent; buffer consumed up to error)
//   tuple    = inner payload: ("async"/"ws"/"stream"/"async_di", ...)
//              or InlineResult — buffer already consumed past this item
//
// All non-tuple returns are small cached ints (Python caches -5..256) → zero allocation
// ═══════════════════════════════════════════════════════════════════════════════
static PyObject* CoreApp_handle_http_batch_v2(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs < 2 || nargs > 3) {
        PyErr_SetString(PyExc_TypeError, "handle_http_batch_v2 requires 2-3 args");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];
    PyObject* transport  = args[1];
    int sock_fd = -1;
    if (nargs >= 3) {
        sock_fd = (int)PyLong_AsLong(args[2]);
        if (sock_fd == -1 && PyErr_Occurred()) { PyErr_Clear(); sock_fd = -1; }
    }

    // Non-capsule path: delegate to v1 for backward compat (never hit in practice)
    if (UNLIKELY(!PyCapsule_CheckExact(buffer_obj))) {
        PyObject* v1_args[3] = {buffer_obj, transport,
                                 sock_fd >= 0 ? PyLong_FromLong(sock_fd) : Py_None};
        PyObject* res = CoreApp_handle_http_batch(self, v1_args, 3);
        if (sock_fd >= 0) Py_DECREF(v1_args[2]);
        return res;
    }

    auto* hb = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(buffer_obj, HTTP_BUF_CAPSULE_NAME));
    if (UNLIKELY(!hb)) return nullptr;

    // Re-arm TCP_QUICKACK once for the whole batch (one syscall per data_received).
    if (sock_fd >= 0) platform_rearm_quickack(sock_fd);

    const char* raw_base = (const char*)hb->data();
    Py_ssize_t  raw_len  = (Py_ssize_t)hb->size();

    if (raw_len == 0) return PyLong_FromLong(-1);  // empty buffer — need more data

    Py_ssize_t prefix = 0;  // bytes consumed by sync requests so far

    while (true) {
        PyObject* r = dispatch_one_request(
            self, raw_base + prefix, raw_len - prefix, transport, sock_fd);

        if (LIKELY(r == Py_True)) {
            // Sync request done — accumulate and loop.
            prefix += self->last_consumed;
            Py_DECREF(r);
            continue;
        }

        if (r == Py_None) {
            // No more complete requests — consume what was processed.
            Py_DECREF(r);
            if (prefix > 0) {
                hb->consume((size_t)prefix);
                // 0 = empty (connection now idle — Python updates ka_deadline)
                // 1 = partial request still in buffer — more data expected
                return PyLong_FromLong(hb->size() > 0 ? 1 : 0);
            }
            return PyLong_FromLong(-1);  // no progress — need more data (cached)
        }

        if (r == Py_False) {
            // Parse error — 400 already sent.
            Py_DECREF(r);
            if (prefix > 0) hb->consume((size_t)prefix);
            return PyLong_FromLong(-2);  // error (cached int, no allocation)
        }

        // Async/WS/Pydantic: (consumed, inner) — consume buffer, return inner directly.
        if (LIKELY(PyTuple_Check(r) && PyTuple_GET_SIZE(r) == 2)) {
            Py_ssize_t total_consumed = prefix + self->last_consumed;
            hb->consume((size_t)total_consumed);
            // Steal ref to inner, release outer wrapper — no PyLong alloc needed.
            PyObject* inner = PyTuple_GET_ITEM(r, 1);
            Py_INCREF(inner);
            Py_DECREF(r);
            return inner;
        }

        // Unexpected shape — return as-is without consuming (safe fallback).
        return r;
    }
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

// ═══════════════════════════════════════════════════════════════════════════════
// write_async_result — direct-write serialized result for async endpoints
//
// Called from _handle_async after awaiting the coroutine.
// Serializes result + writes directly to sock_fd (or transport if EAGAIN).
// Eliminates 1 PyBytes allocation + 1 transport.write() call vs build_response.
//
// args[0] = result (Python object — dict, list, str, int, Pydantic, Response)
// args[1] = transport
// args[2] = status_code (int)
// args[3] = keep_alive (bool)
// args[4] = sock_fd (int)
//
// Returns:
//   True  — response written successfully (non-streaming)
//   None  — streaming response, Python must call _write_chunked_streaming
// ═══════════════════════════════════════════════════════════════════════════════
static PyObject* CoreApp_write_async_result(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs != 5) {
        PyErr_SetString(PyExc_TypeError, "write_async_result requires 5 args");
        return nullptr;
    }

    PyObject* content   = args[0];
    PyObject* transport = args[1];
    int status_code     = (int)PyLong_AsLong(args[2]);
    if (status_code == -1 && PyErr_Occurred()) return nullptr;
    bool keep_alive     = PyObject_IsTrue(args[3]);
    int sock_fd         = (int)PyLong_AsLong(args[4]);
    if (sock_fd == -1 && PyErr_Occurred()) { PyErr_Clear(); sock_fd = -1; }

    // Note: Python caller gates to dict/list only — no need to check body_iterator.
    // Use the same fused serialize+write path as sync endpoints:
    // scatter-gather writev(sock_fd) → zero intermediate PyBytes.
    int rc = serialize_json_and_write_response(
        sock_fd, transport, content, status_code, keep_alive,
        nullptr, 0,   // no accept-encoding (async caller doesn't have it)
        nullptr, nullptr, 0,  // no CORS
        false);       // not a HEAD request

    if (rc >= 0) {
        Py_RETURN_TRUE;
    }

    // Serialization failed (non-JSON-serializable value in dict/list — application error).
    // Clear the exception; caller sees True and connection stays open.
    PyErr_Clear();
    Py_RETURN_TRUE;
}

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
// build_response_from_any — unified type dispatch + serialization in C++
//
// Takes: (raw_result, status_code, keep_alive)
// Returns: HTTP response bytes (PyBytes) for JSON-serializable / Response objects,
//          Py_None if raw_result is a StreamingResponse (Python must handle it),
//          NULL on error.
// Handles: dict, list, str, int, float, bool, None, Response objects, Pydantic models
// ═══════════════════════════════════════════════════════════════════════════════

static PyObject* CoreApp_build_response_from_any(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs != 3) {
        PyErr_SetString(PyExc_TypeError, "build_response_from_any requires 3 args");
        return nullptr;
    }

    PyObject* raw = args[0];
    int status_code = (int)PyLong_AsLong(args[1]);
    if (status_code == -1 && PyErr_Occurred()) return nullptr;
    bool keep_alive = PyObject_IsTrue(args[2]);

    // FAST PATH: JSON-serializable primitives (90%+ of responses)
    if (PyDict_Check(raw) || PyList_Check(raw) || PyUnicode_Check(raw) ||
        PyLong_Check(raw) || PyFloat_Check(raw) || PyBool_Check(raw) ||
        PyTuple_Check(raw) || raw == Py_None) {
        return CoreApp_build_response(self, args, nargs);
    }

    // StreamingResponse: has body_iterator → return None, let Python handle async iteration
    if (s_attr_body_iterator && PyObject_HasAttr(raw, s_attr_body_iterator)) {
        Py_RETURN_NONE;
    }

    // Response objects (HTMLResponse, etc.): has .body + .status_code
    if (s_attr_body && s_attr_status_code &&
        PyObject_HasAttr(raw, s_attr_body) && PyObject_HasAttr(raw, s_attr_status_code)) {
        PyRef body_attr(PyObject_GetAttr(raw, s_attr_body));
        PyRef sc_attr(PyObject_GetAttr(raw, s_attr_status_code));
        if (body_attr && PyBytes_Check(body_attr.get()) && sc_attr && PyLong_Check(sc_attr.get())) {
            int resp_sc = (int)PyLong_AsLong(sc_attr.get());
            char* resp_body; Py_ssize_t resp_body_len;
            PyBytes_AsStringAndSize(body_attr.get(), &resp_body, &resp_body_len);

            PyRef raw_hdrs;
            if (s_attr_raw_headers && PyObject_HasAttr(raw, s_attr_raw_headers)) {
                raw_hdrs = PyRef(PyObject_GetAttr(raw, s_attr_raw_headers));
            } else if (s_attr_raw_headers2) {
                raw_hdrs = PyRef(PyObject_GetAttr(raw, s_attr_raw_headers2));
            } else {
                raw_hdrs = PyRef(PyObject_GetAttrString(raw, "raw_headers"));
            }
            auto buf = acquire_buffer();
            buf.reserve(256 + (size_t)resp_body_len);

            // Status line
            if (resp_sc > 0 && resp_sc < 600 && s_status_lines[resp_sc].data) {
                const auto& sl = s_status_lines[resp_sc];
                buf_append(buf, sl.data, sl.len - 2);
            } else {
                static const char prefix[] = "HTTP/1.1 ";
                buf_append(buf, prefix, sizeof(prefix) - 1);
                char sc_buf[8];
                int sn = fast_i64_to_buf(sc_buf, resp_sc);
                buf_append(buf, sc_buf, sn);
                buf.push_back(' ');
                const char* reason = status_reason(resp_sc);
                buf_append(buf, reason, strlen(reason));
            }

            // Headers from raw_headers
            bool saw_content_length2 = false;
            if (raw_hdrs && PyList_Check(raw_hdrs.get())) {
                Py_ssize_t nhdr = PyList_GET_SIZE(raw_hdrs.get());
                for (Py_ssize_t hi = 0; hi < nhdr; hi++) {
                    PyObject* htuple = PyList_GET_ITEM(raw_hdrs.get(), hi);
                    if (PyTuple_Check(htuple) && PyTuple_GET_SIZE(htuple) >= 2) {
                        PyObject* hname = PyTuple_GET_ITEM(htuple, 0);
                        PyObject* hval = PyTuple_GET_ITEM(htuple, 1);
                        if (PyBytes_Check(hname) && PyBytes_Check(hval)) {
                            buf_append(buf, "\r\n", 2);
                            buf_append(buf, PyBytes_AS_STRING(hname), (size_t)PyBytes_GET_SIZE(hname));
                            buf_append(buf, ": ", 2);
                            buf_append(buf, PyBytes_AS_STRING(hval), (size_t)PyBytes_GET_SIZE(hval));
                            if (!saw_content_length2) {
                                Py_ssize_t hn_len = PyBytes_GET_SIZE(hname);
                                const char* hn_str = PyBytes_AS_STRING(hname);
                                if (hn_len == 14 && strncasecmp(hn_str, "content-length", 14) == 0) {
                                    saw_content_length2 = true;
                                }
                            }
                        }
                    }
                }
            }
            if (!saw_content_length2) {
                char cl_buf[32];
                int cl_len = snprintf(cl_buf, sizeof(cl_buf), "\r\ncontent-length: %zu", (size_t)resp_body_len);
                buf_append(buf, cl_buf, (size_t)cl_len);
            }

            // Connection header + end
            static const char CONN_KA[] = "\r\nconnection: keep-alive\r\n\r\n";
            static const char CONN_CLOSE[] = "\r\nconnection: close\r\n\r\n";
            if (keep_alive) {
                buf_append(buf, CONN_KA, sizeof(CONN_KA) - 1);
            } else {
                buf_append(buf, CONN_CLOSE, sizeof(CONN_CLOSE) - 1);
            }
            buf_append(buf, resp_body, resp_body_len);

            PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
            release_buffer(std::move(buf));
            return result;
        }
        // body not bytes — fall through to Pydantic/fallback
    }

    // Pydantic model: use model_dump_json(by_alias=True) → JSON bytes directly.
    // Faster than model_dump(mode='json') + yyjson re-serialization.
    // by_alias=True matches AstraAPI's jsonable_encoder default behavior.
    {
        if (!s_mdj) s_mdj = PyUnicode_InternFromString("model_dump_json");
        if (!s_by_alias_kw) s_by_alias_kw = PyUnicode_InternFromString("by_alias");
        if (PyObject_HasAttr(raw, s_mdj)) {
            PyRef mdj_method(PyObject_GetAttr(raw, s_mdj));
            if (mdj_method) {
                PyRef kw(PyDict_New());
                if (kw) PyDict_SetItem(kw.get(), s_by_alias_kw, Py_True);
                PyRef json_bytes(kw ? PyObject_Call(mdj_method.get(), g_empty_tuple, kw.get())
                                    : PyObject_CallNoArgs(mdj_method.get()));
                if (json_bytes) {
                    const char* body_data = nullptr;
                    Py_ssize_t body_len = 0;
                    if (PyBytes_Check(json_bytes.get())) {
                        body_data = PyBytes_AS_STRING(json_bytes.get());
                        body_len  = PyBytes_GET_SIZE(json_bytes.get());
                    } else if (PyUnicode_Check(json_bytes.get())) {
                        body_data = PyUnicode_AsUTF8AndSize(json_bytes.get(), &body_len);
                    }
                    if (body_data) {
                        return build_http_response_bytes(
                            status_code, body_data, (size_t)body_len, keep_alive);
                    }
                }
                PyErr_Clear();
            }
        }
    }

    // Ultimate fallback: try to serialize whatever it is
    return CoreApp_build_response(self, args, nargs);
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
    .tp_name = "_astraapi_core.PreparedRequest",
    .tp_basicsize = sizeof(PreparedRequestObject),
    .tp_dealloc = (destructor)PreparedRequest_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
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
    if (self->force_close) req.keep_alive = false;

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

    // ── Route matching (routes always frozen after startup) ────
    if (UNLIKELY(!self->routes_frozen.load(std::memory_order_acquire))) {
        std::unique_lock wlock(self->routes_mutex);
        self->routes_frozen.store(true, std::memory_order_release);
    }
    auto match = self->router.at(req.path.data, req.path.len);
    if (!match) {
            PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive));
        if (resp) return make_consumed_obj(self, req.total_consumed, resp.release());
        return make_consumed_true(self, req.total_consumed);
    }

    int idx = match->route_index;
    if (idx < 0 || idx >= (int)self->routes.size()) {
            PyRef resp(build_http_error_response(404, "Not Found", req.keep_alive));
        if (resp) return make_consumed_obj(self, req.total_consumed, resp.release());
        return make_consumed_true(self, req.total_consumed);
    }

    const auto& route = self->routes[idx];

    // ── Method check (O(1) bitmask) ────────────────────────────────────
    if (route.method_mask) {
        uint8_t req_method = method_str_to_bit(req.method.data, req.method.len);
        if (!(route.method_mask & req_method)) {
                    PyRef resp(build_http_error_response(405, "Method Not Allowed", req.keep_alive));
            if (resp) return make_consumed_obj(self, req.total_consumed, resp.release());
            return make_consumed_true(self, req.total_consumed);
        }
    }

    if (!route.fast_spec) {
            PyRef resp(build_http_error_response(500, "Route not configured", req.keep_alive));
        if (resp) return make_consumed_obj(self, req.total_consumed, resp.release());
        return make_consumed_true(self, req.total_consumed);
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
                    norm_buf[j] = s_header_norm[(unsigned char)hdr.name.data[j]];
                }
                std::string_view normalized(norm_buf, norm_len);
                auto hit = spec.header_map.find(normalized);
                if (hit != spec.header_map.end()) {
                    const auto& fs = spec.header_specs[hit->second];
                    {
                        PyRef py_val(PyUnicode_FromStringAndSize(hdr.value.data, hdr.value.len));
                        if (py_val) {
                            if (fs.is_sequence) {
                                PyObject* existing = PyDict_GetItem(kwargs.get(), fs.py_field_name);
                                if (existing && PyList_Check(existing)) {
                                    PyList_Append(existing, py_val.get());
                                } else {
                                    PyRef lst(PyList_New(1));
                                    if (lst) {
                                        Py_INCREF(py_val.get());
                                        PyList_SET_ITEM(lst.get(), 0, py_val.get());
                                        PyDict_SetItem(kwargs.get(), fs.py_field_name, lst.get());
                                    }
                                }
                            } else {
                                PyDict_SetItem(kwargs.get(), fs.py_field_name, py_val.get());
                            }
                        }
                    }
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
            if (spec.embed_body_fields && kwargs) {
                // OPT: merge yyjson keys directly into kwargs in one pass
                PyObject* full_dict = nullptr;
                if (yyjson_doc_merge_to_dict(doc, kwargs.get(), &full_dict) == 0 && full_dict) {
                    json_body_obj = full_dict;
                }
            } else {
                PyRef parsed(yyjson_doc_to_pyobject(doc));
                if (parsed) {
                    json_body_obj = parsed.release();
                } else {
                    PyErr_Clear();
                }
            }
        }

        if (json_body_obj != Py_None && spec.py_body_param_name) {
            if (!spec.embed_body_fields) {
                PyDict_SetItem(kwargs.get(), spec.py_body_param_name, json_body_obj);
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

        return make_consumed_obj(self, req.total_consumed, (PyObject*)ir);
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

    return make_consumed_obj(self, req.total_consumed, (PyObject*)prep);
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
    // Pre-compute shard index for rate limiting (avoids hash per request)
    self->current_shard_idx = std::hash<std::string>{}(self->current_client_ip) % RATE_LIMIT_SHARDS;
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

// ── Warmup — exercise hot-path code to warm instruction caches ──────────────
// Called once at server startup. Eliminates ~164ms first-request spike.

static PyObject* CoreApp_warmup(CoreAppObject* self, PyObject*) {
    // 1. Warm HTTP parser (llhttp state machine instructions)
    const char* dummy = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ParsedHttpRequest req = {};
    parse_http_request(dummy, strlen(dummy), &req);

    // 2. Warm buffer pool acquire/release cycle
    auto buf = acquire_buffer();
    buf_append(buf, "HTTP/1.1 200 OK\r\n", 17);
    release_buffer(std::move(buf));

    // 3. Warm JSON serializer (yyjson write path)
    PyRef d(PyDict_New());
    if (d) {
        PyRef k(PyUnicode_InternFromString("s"));
        PyRef v(PyUnicode_InternFromString("ok"));
        if (k && v) {
            PyDict_SetItem(d.get(), k.get(), v.get());
            PyRef json(serialize_to_json_pybytes(d.get()));
            (void)json;
        }
    }

    // 4. Warm HTTP response builder
    {
        const char* body = "{\"s\":\"ok\"}";
        PyRef resp(build_http_response_bytes(200, body, 10, true));
        (void)resp;
    }

    PyErr_Clear();
    Py_RETURN_NONE;
}

// ── Method table ────────────────────────────────────────────────────────────

// ═══════════════════════════════════════════════════════════════════════════════
// handle_http_append_and_dispatch — fused append + dispatch (METH_FASTCALL)
//
// Combines _http_buf_append + handle_http_batch_v2 into a single Python→C++ call.
// Eliminates one Python→C++ boundary crossing per data_received() invocation.
//
// args[0] = buffer capsule (HttpConnectionBuffer)
// args[1] = data (bytes-like)   — appended into buffer first
// args[2] = transport
// args[3] = sock_fd (int, optional, default -1)
//
// Returns:
//   False  (PyFalse)  = buffer overflow (413 logic should be handled by caller)
//   int ≥ 0           = sync batch done; value = remaining bytes in buffer
//   None              = need more data
//   False (as parse)  = parse error (400 sent)
//   tuple             = inner async/ws/stream/InlineResult payload
// ═══════════════════════════════════════════════════════════════════════════════
static PyObject* CoreApp_handle_http_append_and_dispatch(
    CoreAppObject* self, PyObject* const* args, Py_ssize_t nargs)
{
    if (nargs < 3 || nargs > 4) {
        PyErr_SetString(PyExc_TypeError, "handle_http_append_and_dispatch requires 3-4 args");
        return nullptr;
    }

    PyObject* buffer_obj = args[0];
    PyObject* data_obj   = args[1];
    PyObject* transport  = args[2];
    int sock_fd = -1;
    if (nargs >= 4) {
        sock_fd = (int)PyLong_AsLong(args[3]);
        if (sock_fd == -1 && PyErr_Occurred()) { PyErr_Clear(); sock_fd = -1; }
    }

    // ── Append data to buffer ──────────────────────────────────────────────
    // Fast path: capsule + bytes/bytearray
    if (UNLIKELY(!PyCapsule_CheckExact(buffer_obj))) {
        // Fallback: shouldn't happen in normal operation
        PyErr_SetString(PyExc_TypeError, "buffer must be HttpConnectionBuffer capsule");
        return nullptr;
    }

    auto* hb = static_cast<HttpConnectionBuffer*>(
        PyCapsule_GetPointer(buffer_obj, HTTP_BUF_CAPSULE_NAME));
    if (UNLIKELY(!hb)) return nullptr;

    // Append data (bytes or bytearray) with minimal overhead
    bool append_ok;
    if (LIKELY(PyBytes_CheckExact(data_obj))) {
        append_ok = hb->append(
            reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(data_obj)),
            (size_t)PyBytes_GET_SIZE(data_obj));
    } else if (data_obj == Py_None) {
        // None data: nothing to append, just dispatch
        append_ok = true;
    } else {
        // Slow path: bytearray, memoryview, etc.
        Py_buffer view;
        if (PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) < 0) return nullptr;
        append_ok = hb->append(static_cast<const uint8_t*>(view.buf), (size_t)view.len);
        PyBuffer_Release(&view);
    }

    if (UNLIKELY(!append_ok)) {
        // Buffer overflow — return -3 so Python int checks cover all error codes
        return PyLong_FromLong(-3);
    }

    // ── Dispatch (same as handle_http_batch_v2) ────────────────────────────
    if (sock_fd >= 0) platform_rearm_quickack(sock_fd);

    const char* raw_base = (const char*)hb->data();
    Py_ssize_t  raw_len  = (Py_ssize_t)hb->size();

    if (raw_len == 0) return PyLong_FromLong(-1);  // empty — need more data

    Py_ssize_t prefix = 0;

    while (true) {
        PyObject* r = dispatch_one_request(
            self, raw_base + prefix, raw_len - prefix, transport, sock_fd);

        if (LIKELY(r == Py_True)) {
            prefix += self->last_consumed;
            Py_DECREF(r);
            continue;
        }

        if (r == Py_None) {
            Py_DECREF(r);
            if (prefix > 0) {
                hb->consume((size_t)prefix);
                // 0 = buffer empty (connection idle), 1 = partial request pending
                return PyLong_FromLong(hb->size() > 0 ? 1 : 0);
            }
            return PyLong_FromLong(-1);  // no progress — need more data (cached)
        }

        if (r == Py_False) {
            Py_DECREF(r);
            if (prefix > 0) hb->consume((size_t)prefix);
            return PyLong_FromLong(-2);  // parse error (cached)
        }

        if (LIKELY(PyTuple_Check(r) && PyTuple_GET_SIZE(r) == 2)) {
            Py_ssize_t total_consumed = prefix + self->last_consumed;
            hb->consume((size_t)total_consumed);
            PyObject* inner = PyTuple_GET_ITEM(r, 1);
            Py_INCREF(inner);
            Py_DECREF(r);
            return inner;
        }

        return r;
    }
}

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
    {"set_type_exception_handlers", (PyCFunction)CoreApp_set_type_exception_handlers, METH_VARARGS, nullptr},
    {"set_has_http_middleware", (PyCFunction)CoreApp_set_has_http_middleware, METH_VARARGS, nullptr},
    {"set_https_redirect", (PyCFunction)CoreApp_set_https_redirect, METH_VARARGS, nullptr},
    // C++ HTTP server path — bypass ASGI entirely
    {"handle_http", (PyCFunction)(void(*)(void))CoreApp_handle_http, METH_FASTCALL, nullptr},
    {"handle_http_batch", (PyCFunction)(void(*)(void))CoreApp_handle_http_batch, METH_FASTCALL, nullptr},
    {"handle_http_batch_v2", (PyCFunction)(void(*)(void))CoreApp_handle_http_batch_v2, METH_FASTCALL, nullptr},
    {"handle_http_append_and_dispatch", (PyCFunction)(void(*)(void))CoreApp_handle_http_append_and_dispatch, METH_FASTCALL, nullptr},
    {"serialize_and_write_http", (PyCFunction)(void(*)(void))CoreApp_serialize_and_write_http, METH_FASTCALL, nullptr},
    // Direct-write for async endpoints — serializes + writes to fd, no PyBytes alloc
    {"write_async_result", (PyCFunction)(void(*)(void))CoreApp_write_async_result, METH_FASTCALL, nullptr},
    // Returns complete HTTP response as single PyBytes (no transport calls)
    {"build_response", (PyCFunction)(void(*)(void))CoreApp_build_response, METH_FASTCALL, nullptr},
    // Unified type dispatch + serialization — handles dict/list/Response/Pydantic, returns None for StreamingResponse
    {"build_response_from_any", (PyCFunction)(void(*)(void))CoreApp_build_response_from_any, METH_FASTCALL, nullptr},
    // Non-blocking parse+route — returns PreparedRequest for async dispatch
    {"parse_and_route", (PyCFunction)(void(*)(void))CoreApp_parse_and_route, METH_FASTCALL, nullptr},
    {"freeze_routes", (PyCFunction)CoreApp_freeze_routes, METH_NOARGS, nullptr},
    {"set_openapi_schema", (PyCFunction)CoreApp_set_openapi_schema, METH_O, nullptr},
    {"set_urls", (PyCFunction)CoreApp_set_urls, METH_VARARGS, nullptr},
    {"set_swagger_ui_parameters", (PyCFunction)CoreApp_set_swagger_ui_parameters, METH_O, nullptr},
    // C++ native middleware support
    {"configure_rate_limit", (PyCFunction)CoreApp_configure_rate_limit, METH_VARARGS, nullptr},
    {"set_client_ip", (PyCFunction)CoreApp_set_client_ip, METH_O, nullptr},
    {"set_post_response_hook", (PyCFunction)CoreApp_set_post_response_hook, METH_O, nullptr},
    {"warmup", (PyCFunction)CoreApp_warmup, METH_NOARGS, nullptr},
    {nullptr}
};

static PyMemberDef CoreApp_members[] = {
    {"last_consumed", Py_T_PYSSIZET, offsetof(CoreAppObject, last_consumed), Py_READONLY, nullptr},
    {"force_close", Py_T_INT, offsetof(CoreAppObject, force_close), 0, nullptr},
    {"redirect_slashes", Py_T_BOOL, offsetof(CoreAppObject, redirect_slashes), 0, nullptr},
    {nullptr}
};

PyTypeObject CoreAppType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_astraapi_core.CoreApp",
    .tp_basicsize = sizeof(CoreAppObject),
    .tp_dealloc = (destructor)CoreApp_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = CoreApp_methods,
    .tp_members = CoreApp_members,
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
        buf_append(buf, sl.data, sl.len);
    } else {
        static const char prefix[] = "HTTP/1.1 ";
        buf_append(buf, prefix, sizeof(prefix) - 1);
        char sc_buf[8];
        int sn = fast_i64_to_buf(sc_buf, status_code);
        buf_append(buf, sc_buf, sn);
        buf.push_back(' ');
        const char* reason = status_reason(status_code);
        size_t rlen = strlen(reason);
        buf_append(buf, reason, rlen);
        buf_append(buf, "\r\n", 2);
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

        buf_append(buf, kstr, klen);
        buf_append(buf, ": ", 2);
        buf_append(buf, vstr, vlen);
        buf_append(buf, "\r\n", 2);

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
        buf_append(buf, cl_pre, sizeof(cl_pre) - 1);
        char cl_buf[20];
        int cl_n = fast_i64_to_buf(cl_buf, (long long)body_len);
        buf_append(buf, cl_buf, cl_n);
        buf_append(buf, "\r\n", 2);
    }

    // Connection header
    if (keep_alive) {
        static const char ka[] = "connection: keep-alive\r\n";
        buf_append(buf, ka, sizeof(ka) - 1);
    } else {
        static const char cl[] = "connection: close\r\n";
        buf_append(buf, cl, sizeof(cl) - 1);
    }

    // End of headers + body
    buf_append(buf, "\r\n", 2);
    buf_append(buf, (const char*)body.buf, body_len);
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
