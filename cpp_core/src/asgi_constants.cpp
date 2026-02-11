#include "asgi_constants.hpp"
#include "pyref.hpp"
#include <cstring>

// ── String constants ────────────────────────────────────────────────────────
PyObject* g_str_type = nullptr;
PyObject* g_str_method = nullptr;
PyObject* g_str_path = nullptr;
PyObject* g_str_query_string = nullptr;
PyObject* g_str_headers = nullptr;
PyObject* g_str_status = nullptr;
PyObject* g_str_body = nullptr;
PyObject* g_str_root_path = nullptr;
PyObject* g_str_scheme = nullptr;
PyObject* g_str_server = nullptr;
PyObject* g_str_http = nullptr;
PyObject* g_str_https = nullptr;
PyObject* g_str_scope_type = nullptr;

PyObject* g_str_http_response_start = nullptr;
PyObject* g_str_http_response_body = nullptr;
PyObject* g_str_http_request = nullptr;
PyObject* g_str_http_disconnect = nullptr;
PyObject* g_str_websocket = nullptr;

// ── Bytes constants ─────────────────────────────────────────────────────────
PyObject* g_bytes_content_type = nullptr;
PyObject* g_bytes_content_length = nullptr;
PyObject* g_bytes_content_type_json = nullptr;
PyObject* g_bytes_content_type_text = nullptr;
PyObject* g_bytes_content_type_html = nullptr;
PyObject* g_bytes_cookie = nullptr;
PyObject* g_bytes_authorization = nullptr;
PyObject* g_bytes_accept_encoding = nullptr;

// ── Common strings ──────────────────────────────────────────────────────────
PyObject* g_str_content_type = nullptr;
PyObject* g_str_content_length = nullptr;
PyObject* g_str_application_json = nullptr;
PyObject* g_str_text_plain = nullptr;
PyObject* g_str_text_html = nullptr;
PyObject* g_str_none_str = nullptr;

// ── Pre-built ASGI response objects ─────────────────────────────────────────
PyObject* g_ct_json_header_pair = nullptr;
PyObject* g_status_200 = nullptr;
PyObject* g_status_201 = nullptr;
PyObject* g_status_204 = nullptr;
PyObject* g_status_301 = nullptr;
PyObject* g_status_400 = nullptr;
PyObject* g_status_404 = nullptr;
PyObject* g_status_422 = nullptr;
PyObject* g_status_500 = nullptr;
PyObject* g_start_msg_template = nullptr;
PyObject* g_body_msg_template = nullptr;
PyObject* g_empty_tuple = nullptr;
PyObject* g_500_start = nullptr;
PyObject* g_500_body = nullptr;

// Fast integer formatting (~5x faster than snprintf for typical values)
int fast_i64_to_buf(char* buf, long long val) {
    char tmp[21];
    char* p = tmp + 20;
    bool neg = val < 0;
    unsigned long long uv = neg ? -(unsigned long long)val : (unsigned long long)val;
    do { *--p = '0' + (char)(uv % 10); uv /= 10; } while (uv);
    if (neg) *--p = '-';
    int len = (int)(tmp + 20 - p);
    memcpy(buf, p, len);
    return len;
}

int init_asgi_constants() {
    // Interned strings — identity comparison works (pointer ==)
    g_str_type = PyUnicode_InternFromString("type");
    g_str_method = PyUnicode_InternFromString("method");
    g_str_path = PyUnicode_InternFromString("path");
    g_str_query_string = PyUnicode_InternFromString("query_string");
    g_str_headers = PyUnicode_InternFromString("headers");
    g_str_status = PyUnicode_InternFromString("status");
    g_str_body = PyUnicode_InternFromString("body");
    g_str_root_path = PyUnicode_InternFromString("root_path");
    g_str_scheme = PyUnicode_InternFromString("scheme");
    g_str_server = PyUnicode_InternFromString("server");
    g_str_http = PyUnicode_InternFromString("http");
    g_str_https = PyUnicode_InternFromString("https");
    g_str_scope_type = PyUnicode_InternFromString("scope_type");

    g_str_http_response_start = PyUnicode_InternFromString("http.response.start");
    g_str_http_response_body = PyUnicode_InternFromString("http.response.body");
    g_str_http_request = PyUnicode_InternFromString("http.request");
    g_str_http_disconnect = PyUnicode_InternFromString("http.disconnect");
    g_str_websocket = PyUnicode_InternFromString("websocket");

    // Bytes constants — pre-allocated, never freed
    g_bytes_content_type = PyBytes_FromStringAndSize("content-type", 12);
    g_bytes_content_length = PyBytes_FromStringAndSize("content-length", 14);
    g_bytes_content_type_json = PyBytes_FromStringAndSize("application/json", 16);
    g_bytes_content_type_text = PyBytes_FromStringAndSize("text/plain; charset=utf-8", 25);
    g_bytes_content_type_html = PyBytes_FromStringAndSize("text/html; charset=utf-8", 24);
    g_bytes_cookie = PyBytes_FromStringAndSize("cookie", 6);
    g_bytes_authorization = PyBytes_FromStringAndSize("authorization", 13);
    g_bytes_accept_encoding = PyBytes_FromStringAndSize("accept-encoding", 15);

    // Common strings
    g_str_content_type = PyUnicode_InternFromString("content-type");
    g_str_content_length = PyUnicode_InternFromString("content-length");
    g_str_application_json = PyUnicode_InternFromString("application/json");
    g_str_text_plain = PyUnicode_InternFromString("text/plain");
    g_str_text_html = PyUnicode_InternFromString("text/html");
    g_str_none_str = PyUnicode_InternFromString("None");

    // ── Pre-built ASGI response objects ────────────────────────────────────
    // These are created once and reused for every request (immortal objects).

    // [b"content-type", b"application/json"] — immutable tuple to prevent
    // shared mutable state corruption across requests
    g_ct_json_header_pair = PyTuple_New(2);
    if (!g_ct_json_header_pair) return -1;
    Py_INCREF(g_bytes_content_type);
    Py_INCREF(g_bytes_content_type_json);
    PyTuple_SET_ITEM(g_ct_json_header_pair, 0, g_bytes_content_type);
    PyTuple_SET_ITEM(g_ct_json_header_pair, 1, g_bytes_content_type_json);

    // Cached status codes
    g_status_200 = PyLong_FromLong(200);
    g_status_201 = PyLong_FromLong(201);
    g_status_204 = PyLong_FromLong(204);
    g_status_301 = PyLong_FromLong(301);
    g_status_400 = PyLong_FromLong(400);
    g_status_404 = PyLong_FromLong(404);
    g_status_422 = PyLong_FromLong(422);
    g_status_500 = PyLong_FromLong(500);
    if (!g_status_200 || !g_status_201 || !g_status_204 || !g_status_301 ||
        !g_status_400 || !g_status_404 || !g_status_422 || !g_status_500) return -1;

    // {"type": "http.response.start"} — template dict (will be copied per request)
    g_start_msg_template = PyDict_New();
    if (!g_start_msg_template) return -1;
    PyDict_SetItem(g_start_msg_template, g_str_type, g_str_http_response_start);

    // {"type": "http.response.body"} — template dict
    g_body_msg_template = PyDict_New();
    if (!g_body_msg_template) return -1;
    PyDict_SetItem(g_body_msg_template, g_str_type, g_str_http_response_body);

    // Empty tuple for PyObject_Call(endpoint, (), kwargs)
    g_empty_tuple = PyTuple_New(0);
    if (!g_empty_tuple) return -1;

    // Pre-built 500 response dicts
    g_500_start = PyDict_New();
    if (!g_500_start) return -1;
    PyDict_SetItem(g_500_start, g_str_type, g_str_http_response_start);
    PyRef status_500(PyLong_FromLong(500));
    PyDict_SetItem(g_500_start, g_str_status, status_500.get());
    PyRef h500_list(PyList_New(1));
    PyRef h500_ct(PyList_New(2));
    Py_INCREF(g_bytes_content_type);
    Py_INCREF(g_bytes_content_type_json);
    PyList_SET_ITEM(h500_ct.get(), 0, g_bytes_content_type);
    PyList_SET_ITEM(h500_ct.get(), 1, g_bytes_content_type_json);
    PyList_SET_ITEM(h500_list.get(), 0, h500_ct.release());
    PyDict_SetItem(g_500_start, g_str_headers, h500_list.get());

    g_500_body = PyDict_New();
    if (!g_500_body) return -1;
    PyDict_SetItem(g_500_body, g_str_type, g_str_http_response_body);
    PyRef body_500(PyBytes_FromString("{\"detail\":\"Internal Server Error\"}"));
    PyDict_SetItem(g_500_body, g_str_body, body_500.get());

    // Check all succeeded
    if (!g_str_type || !g_str_method || !g_str_path || !g_str_query_string ||
        !g_str_headers || !g_str_status || !g_str_body ||
        !g_bytes_content_type || !g_bytes_content_length ||
        !g_ct_json_header_pair || !g_status_200 || !g_start_msg_template ||
        !g_body_msg_template || !g_empty_tuple || !g_500_start || !g_500_body) {
        return -1;
    }
    return 0;
}

void cleanup_asgi_constants() {
    // Note: interned strings (PyUnicode_InternFromString) are managed by the
    // intern table and may not be freed by Py_CLEAR, but we still clear our
    // references to be correct. Non-interned objects (PyBytes, PyDict, PyTuple,
    // PyLong, PyList) are properly freed.
    Py_CLEAR(g_str_type);
    Py_CLEAR(g_str_method);
    Py_CLEAR(g_str_path);
    Py_CLEAR(g_str_query_string);
    Py_CLEAR(g_str_headers);
    Py_CLEAR(g_str_status);
    Py_CLEAR(g_str_body);
    Py_CLEAR(g_str_root_path);
    Py_CLEAR(g_str_scheme);
    Py_CLEAR(g_str_server);
    Py_CLEAR(g_str_http);
    Py_CLEAR(g_str_https);
    Py_CLEAR(g_str_scope_type);
    Py_CLEAR(g_str_http_response_start);
    Py_CLEAR(g_str_http_response_body);
    Py_CLEAR(g_str_http_request);
    Py_CLEAR(g_str_http_disconnect);
    Py_CLEAR(g_str_websocket);
    Py_CLEAR(g_bytes_content_type);
    Py_CLEAR(g_bytes_content_length);
    Py_CLEAR(g_bytes_content_type_json);
    Py_CLEAR(g_bytes_content_type_text);
    Py_CLEAR(g_bytes_content_type_html);
    Py_CLEAR(g_bytes_cookie);
    Py_CLEAR(g_bytes_authorization);
    Py_CLEAR(g_bytes_accept_encoding);
    Py_CLEAR(g_str_content_type);
    Py_CLEAR(g_str_content_length);
    Py_CLEAR(g_str_application_json);
    Py_CLEAR(g_str_text_plain);
    Py_CLEAR(g_str_text_html);
    Py_CLEAR(g_str_none_str);
    Py_CLEAR(g_ct_json_header_pair);
    Py_CLEAR(g_status_200);
    Py_CLEAR(g_status_201);
    Py_CLEAR(g_status_204);
    Py_CLEAR(g_status_301);
    Py_CLEAR(g_status_400);
    Py_CLEAR(g_status_404);
    Py_CLEAR(g_status_422);
    Py_CLEAR(g_status_500);
    Py_CLEAR(g_start_msg_template);
    Py_CLEAR(g_body_msg_template);
    Py_CLEAR(g_empty_tuple);
    Py_CLEAR(g_500_start);
    Py_CLEAR(g_500_body);
}
