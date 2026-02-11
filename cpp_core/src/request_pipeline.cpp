#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "asgi_constants.hpp"
#include "json_parser.hpp"
#include "pyref.hpp"
#include <cstring>
#include <string>

// Forward declarations from utils.cpp
extern PyObject* py_parse_query_string(PyObject* self, PyObject* args);
extern PyObject* py_parse_headers_to_dict(PyObject* self, PyObject* args);

// ══════════════════════════════════════════════════════════════════════════════
// process_request(query_string, raw_headers, body, convert_underscores)
// → PyDict with query_params, headers, cookies, is_json, json_body
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_process_request(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "query_string", "raw_headers", "body", "convert_underscores", nullptr
    };

    const char* query_string = "";
    Py_ssize_t query_len = 0;
    PyObject* raw_headers = nullptr;
    PyObject* body = Py_None;
    int convert_underscores = 1;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s#O|Op", (char**)kwlist,
            &query_string, &query_len, &raw_headers, &body, &convert_underscores)) {
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    // ── Parse query string ───────────────────────────────────────────────
    {
        PyRef qs_args(Py_BuildValue("(s#)", query_string, query_len));
        PyRef qp(py_parse_query_string(self, qs_args.get()));
        if (!qp) return nullptr;
        PyDict_SetItemString(result.get(), "query_params", qp.get());
    }

    // ── Parse headers ────────────────────────────────────────────────────
    PyRef headers_dict(PyDict_New());
    PyRef cookies_dict(PyDict_New());
    if (!headers_dict || !cookies_dict) return nullptr;

    bool is_json = false;

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
                if (convert_underscores && c == '-') c = '_';
            }

            PyRef hkey(PyUnicode_FromStringAndSize(norm.c_str(), norm.size()));
            PyRef hval(PyUnicode_FromStringAndSize(val_data, val_len));
            if (!hkey || !hval) return nullptr;
            PyDict_SetItem(headers_dict.get(), hkey.get(), hval.get());

            // Cookie parsing
            if (name_len == 6 && memcmp(name_data, "cookie", 6) == 0) {
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

            // Check content-type for JSON
            if (name_len == 12 && memcmp(name_data, "content-type", 12) == 0) {
                std::string ct_lower(val_data, val_len);
                for (auto& c : ct_lower) if (c >= 'A' && c <= 'Z') c += 32;
                if (ct_lower.find("json") != std::string::npos) is_json = true;
            }
        }
    }

    PyDict_SetItemString(result.get(), "headers", headers_dict.get());
    PyDict_SetItemString(result.get(), "cookies", cookies_dict.get());
    PyDict_SetItemString(result.get(), "is_json", is_json ? Py_True : Py_False);

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

    return result.release();
}
