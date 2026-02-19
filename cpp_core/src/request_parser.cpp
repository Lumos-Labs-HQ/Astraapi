#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "asgi_constants.hpp"
#include "json_parser.hpp"
#include "percent_decode.hpp"
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// parse_request_full — unified single-call request parsing
//
// Signature: parse_request_full(route_id, raw_headers, query_string,
//                                path_params, body, is_form, form_boundary)
// Returns: dict with query_params, headers, cookies, is_json, json_body, etc.
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_request_full(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "route_id", "raw_headers", "query_string", "path_params",
        "body", "is_form", "form_boundary", nullptr
    };

    unsigned long long route_id = 0;
    PyObject* raw_headers = nullptr;
    const char* query_string = "";
    Py_ssize_t query_len = 0;
    PyObject* path_params = Py_None;
    PyObject* body = Py_None;
    int is_form = 0;
    const char* form_boundary = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "KOs#|OpOz", (char**)kwlist,
            &route_id, &raw_headers, &query_string, &query_len,
            &path_params, &body, &is_form, &form_boundary)) {
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    // ── Parse query string ───────────────────────────────────────────────
    PyRef query_params(PyDict_New());
    if (!query_params) return nullptr;

    if (query_len > 0) {
        const char* p = query_string;
        const char* end = query_string + query_len;
        while (p < end) {
            const char* key_start = p;
            const char* eq = nullptr;
            while (p < end && *p != '&') {
                if (*p == '=' && !eq) eq = p;
                p++;
            }
            std::string key, val;
            if (eq) {
                key = percent_decode(key_start, eq - key_start);
                val = percent_decode(eq + 1, p - eq - 1);
            } else {
                key = percent_decode(key_start, p - key_start);
            }
            PyRef pk(PyUnicode_FromStringAndSize(key.c_str(), key.size()));
            PyRef pv(PyUnicode_FromStringAndSize(val.c_str(), val.size()));
            if (!pk || !pv) return nullptr;

            PyObject* existing = PyDict_GetItem(query_params.get(), pk.get());
            if (existing && PyList_Check(existing)) {
                PyList_Append(existing, pv.get());
            } else {
                PyRef lst(PyList_New(1));
                PyList_SET_ITEM(lst.get(), 0, pv.release());
                PyDict_SetItem(query_params.get(), pk.get(), lst.get());
            }
            if (p < end) p++;
        }
    }
    PyDict_SetItemString(result.get(), "query_params", query_params.get());

    // ── Parse headers ────────────────────────────────────────────────────
    PyRef headers_dict(PyDict_New());
    PyRef cookies_dict(PyDict_New());
    if (!headers_dict || !cookies_dict) return nullptr;

    bool is_json = false;
    PyRef origin(Py_None); Py_INCREF(Py_None);
    PyRef host(Py_None); Py_INCREF(Py_None);
    PyRef accept_encoding(Py_None); Py_INCREF(Py_None);
    PyRef content_type_raw(Py_None); Py_INCREF(Py_None);

    if (raw_headers && PyList_Check(raw_headers)) {
        Py_ssize_t nheaders = PyList_GET_SIZE(raw_headers);
        for (Py_ssize_t i = 0; i < nheaders; i++) {
            PyObject* item = PyList_GET_ITEM(raw_headers, i);
            PyObject* name_obj;
            PyObject* value_obj;
            if (PyTuple_Check(item) && PyTuple_GET_SIZE(item) >= 2) {
                name_obj = PyTuple_GET_ITEM(item, 0);
                value_obj = PyTuple_GET_ITEM(item, 1);
            } else if (PyList_Check(item) && PyList_GET_SIZE(item) >= 2) {
                name_obj = PyList_GET_ITEM(item, 0);
                value_obj = PyList_GET_ITEM(item, 1);
            } else continue;

            char* name_data;
            Py_ssize_t name_len;
            char* val_data;
            Py_ssize_t val_len;
            if (PyBytes_AsStringAndSize(name_obj, &name_data, &name_len) < 0) { PyErr_Clear(); continue; }
            if (PyBytes_AsStringAndSize(value_obj, &val_data, &val_len) < 0) { PyErr_Clear(); continue; }

            // Normalize header name
            std::string norm(name_data, name_len);
            for (auto& c : norm) {
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c == '-') c = '_';
            }

            PyRef hkey(PyUnicode_FromStringAndSize(norm.c_str(), norm.size()));
            PyRef hval(PyUnicode_FromStringAndSize(val_data, val_len));
            if (!hkey || !hval) return nullptr;
            PyDict_SetItem(headers_dict.get(), hkey.get(), hval.get());

            // Special headers
            if (name_len == 6 && memcmp(name_data, "cookie", 6) == 0) {
                // Parse cookies
                const char* cp = val_data;
                const char* cend = val_data + val_len;
                while (cp < cend) {
                    while (cp < cend && (*cp == ' ' || *cp == ';')) cp++;
                    const char* ck_s = cp;
                    while (cp < cend && *cp != '=') cp++;
                    if (cp >= cend) break;
                    std::string ck(ck_s, cp - ck_s);
                    cp++;
                    const char* cv_s = cp;
                    while (cp < cend && *cp != ';') cp++;
                    std::string cv(cv_s, cp - cv_s);
                    PyRef ckey(PyUnicode_FromStringAndSize(ck.c_str(), ck.size()));
                    PyRef cval(PyUnicode_FromStringAndSize(cv.c_str(), cv.size()));
                    if (ckey && cval) PyDict_SetItem(cookies_dict.get(), ckey.get(), cval.get());
                }
            }
            else if (name_len == 12 && memcmp(name_data, "content-type", 12) == 0) {
                content_type_raw = PyRef::borrow(hval.get());
                // Check if JSON
                std::string ct_lower(val_data, val_len);
                for (auto& c : ct_lower) if (c >= 'A' && c <= 'Z') c += 32;
                if (ct_lower.find("json") != std::string::npos) is_json = true;
            }
            else if (name_len == 6 && memcmp(name_data, "origin", 6) == 0) {
                origin = PyRef::borrow(hval.get());
            }
            else if (name_len == 4 && memcmp(name_data, "host", 4) == 0) {
                host = PyRef::borrow(hval.get());
            }
            else if (name_len == 15 && memcmp(name_data, "accept-encoding", 15) == 0) {
                accept_encoding = PyRef::borrow(hval.get());
            }
        }
    }

    PyDict_SetItemString(result.get(), "headers", headers_dict.get());
    PyDict_SetItemString(result.get(), "cookies", cookies_dict.get());
    PyDict_SetItemString(result.get(), "is_json", is_json ? Py_True : Py_False);
    PyDict_SetItemString(result.get(), "origin", origin.get());
    PyDict_SetItemString(result.get(), "host", host.get());
    PyDict_SetItemString(result.get(), "accept_encoding", accept_encoding.get());
    PyDict_SetItemString(result.get(), "content_type_raw", content_type_raw.get());

    // ── Parse JSON body (yyjson — zero Python calls) ──────────────────────
    PyObject* json_body = Py_None;
    if (is_json && body != Py_None && PyBytes_Check(body)) {
        char* body_data = nullptr;
        Py_ssize_t body_len = 0;
        if (PyBytes_AsStringAndSize(body, &body_data, &body_len) == 0 && body_len > 0) {
            PyRef parsed(yyjson_parse_to_pyobject(body_data, static_cast<size_t>(body_len)));
            if (parsed) {
                json_body = parsed.release();
            } else {
                PyErr_Clear();
            }
        }
    }
    PyDict_SetItemString(result.get(), "json_body", json_body);
    if (json_body != Py_None) Py_DECREF(json_body);

    // ── Extracted values placeholder ─────────────────────────────────────
    PyRef extracted(PyDict_New());
    PyDict_SetItemString(result.get(), "extracted_values", extracted.get());

    // ── Form data placeholder ────────────────────────────────────────────
    PyRef form_data(PyDict_New());
    PyDict_SetItemString(result.get(), "form_data", form_data.get());

    return result.release();
}
