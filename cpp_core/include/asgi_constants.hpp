#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>

// Pre-interned Python strings — created once at module init, used every request.
// PyUnicode_InternFromString makes pointer comparison work (==, not strcmp).
// This is the SAME interning mechanism CPython uses for attribute lookups.

// ── String constants (PyUnicode) ────────────────────────────────────────────
extern PyObject* g_str_type;
extern PyObject* g_str_method;
extern PyObject* g_str_path;
extern PyObject* g_str_query_string;
extern PyObject* g_str_headers;
extern PyObject* g_str_status;
extern PyObject* g_str_body;
extern PyObject* g_str_root_path;
extern PyObject* g_str_scheme;
extern PyObject* g_str_server;
extern PyObject* g_str_http;
extern PyObject* g_str_https;
extern PyObject* g_str_scope_type;

// ASGI message types
extern PyObject* g_str_http_response_start;
extern PyObject* g_str_http_response_body;
extern PyObject* g_str_http_request;
extern PyObject* g_str_http_disconnect;
extern PyObject* g_str_websocket;

// ── Bytes constants (PyBytes) ───────────────────────────────────────────────
extern PyObject* g_bytes_content_type;
extern PyObject* g_bytes_content_length;
extern PyObject* g_bytes_content_type_json;
extern PyObject* g_bytes_content_type_text;
extern PyObject* g_bytes_content_type_html;
extern PyObject* g_bytes_cookie;
extern PyObject* g_bytes_authorization;
extern PyObject* g_bytes_accept_encoding;

// ── Common header/value strings ─────────────────────────────────────────────
extern PyObject* g_str_content_type;
extern PyObject* g_str_content_length;
extern PyObject* g_str_application_json;
extern PyObject* g_str_text_plain;
extern PyObject* g_str_text_html;
extern PyObject* g_str_none_str;

// ── Pre-built ASGI response objects (created once, reused every request) ────
extern PyObject* g_ct_json_header_pair;   // [b"content-type", b"application/json"]
extern PyObject* g_status_200;            // PyLong(200)
extern PyObject* g_status_201;            // PyLong(201)
extern PyObject* g_status_204;            // PyLong(204)
extern PyObject* g_status_301;            // PyLong(301)
extern PyObject* g_status_400;            // PyLong(400)
extern PyObject* g_status_404;            // PyLong(404)
extern PyObject* g_status_422;            // PyLong(422)
extern PyObject* g_status_500;            // PyLong(500)

// O(1) status code lookup — returns cached PyLong or creates new one
inline PyObject* get_cached_status(int code) {
    switch (code) {
        case 200: Py_INCREF(g_status_200); return g_status_200;
        case 201: Py_INCREF(g_status_201); return g_status_201;
        case 204: Py_INCREF(g_status_204); return g_status_204;
        case 301: Py_INCREF(g_status_301); return g_status_301;
        case 400: Py_INCREF(g_status_400); return g_status_400;
        case 404: Py_INCREF(g_status_404); return g_status_404;
        case 422: Py_INCREF(g_status_422); return g_status_422;
        case 500: Py_INCREF(g_status_500); return g_status_500;
        default: return PyLong_FromLong(code);
    }
}
extern PyObject* g_start_msg_template;    // {"type": "http.response.start"}
extern PyObject* g_body_msg_template;     // {"type": "http.response.body"}
extern PyObject* g_empty_tuple;           // ()
extern PyObject* g_500_start;             // pre-built 500 start dict
extern PyObject* g_500_body;              // pre-built 500 body dict

// Fast integer-to-buffer (no snprintf overhead)
int fast_i64_to_buf(char* buf, long long val);

// Initialize all constants — call once from PyInit__fastapi_core
int init_asgi_constants();

// Clean up all constants — call from module m_free
void cleanup_asgi_constants();
