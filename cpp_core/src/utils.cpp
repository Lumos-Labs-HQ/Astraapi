#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "asgi_constants.hpp"
#include "pyref.hpp"
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

// ══════════════════════════════════════════════════════════════════════════════
// Percent-decoding
// ══════════════════════════════════════════════════════════════════════════════

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static std::string percent_decode(const char* s, size_t len) {
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(s[i]);
        }
    }
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_query_string(query: str) → PyDict {key: [values]}
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_query_string(PyObject* self, PyObject* args) {
    const char* query;
    Py_ssize_t query_len;
    if (!PyArg_ParseTuple(args, "s#", &query, &query_len)) return nullptr;

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    if (query_len == 0) return result.release();

    const char* p = query;
    const char* end = query + query_len;

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

        PyRef py_key(PyUnicode_FromStringAndSize(key.c_str(), key.size()));
        PyRef py_val(PyUnicode_FromStringAndSize(val.c_str(), val.size()));
        if (!py_key || !py_val) return nullptr;

        // Multi-value: dict[key] is a list
        PyObject* existing = PyDict_GetItem(result.get(), py_key.get());  // borrowed
        if (existing && PyList_Check(existing)) {
            PyList_Append(existing, py_val.get());
        } else {
            PyRef new_list(PyList_New(1));
            if (!new_list) return nullptr;
            PyList_SET_ITEM(new_list.get(), 0, py_val.release());  // steals ref
            PyDict_SetItem(result.get(), py_key.get(), new_list.get());
        }

        if (p < end) p++;  // skip '&'
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_headers_to_dict(raw_headers: list, convert_underscores: bool) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_headers_to_dict(PyObject* self, PyObject* args) {
    PyObject* raw_headers;
    int convert_underscores = 1;
    if (!PyArg_ParseTuple(args, "O|p", &raw_headers, &convert_underscores)) return nullptr;

    if (!PyList_Check(raw_headers)) {
        PyErr_SetString(PyExc_TypeError, "expected list");
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    Py_ssize_t n = PyList_GET_SIZE(raw_headers);
    for (Py_ssize_t i = 0; i < n; i++) {
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

        // Get name as string
        const char* name_str;
        Py_ssize_t name_len;
        if (PyBytes_Check(name_obj)) {
            PyBytes_AsStringAndSize(name_obj, (char**)&name_str, &name_len);
        } else if (PyUnicode_Check(name_obj)) {
            name_str = PyUnicode_AsUTF8AndSize(name_obj, &name_len);
            if (!name_str) return nullptr;
        } else continue;

        // Normalize: lowercase, optionally replace - with _
        std::string normalized(name_str, name_len);
        for (auto& c : normalized) {
            if (c >= 'A' && c <= 'Z') c += 32;
            if (convert_underscores && c == '-') c = '_';
        }

        // Get value as string
        PyRef val_str_obj(nullptr);
        if (PyBytes_Check(value_obj)) {
            char* vdata;
            Py_ssize_t vlen;
            PyBytes_AsStringAndSize(value_obj, &vdata, &vlen);
            val_str_obj = PyRef(PyUnicode_FromStringAndSize(vdata, vlen));
        } else if (PyUnicode_Check(value_obj)) {
            Py_INCREF(value_obj);
            val_str_obj = PyRef(value_obj);
        } else continue;

        PyRef key(PyUnicode_FromStringAndSize(normalized.c_str(), normalized.size()));
        if (!key || !val_str_obj) return nullptr;
        PyDict_SetItem(result.get(), key.get(), val_str_obj.get());
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_scope_headers(raw_headers: PyList, convert_underscores: bool) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_scope_headers(PyObject* self, PyObject* args) {
    // Same impl as parse_headers_to_dict — ASGI scope headers are bytes tuples
    return py_parse_headers_to_dict(self, args);
}

// ══════════════════════════════════════════════════════════════════════════════
// batch_extract_params(parsed_query: PyDict, field_specs: PyList) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_batch_extract_params(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"parsed_query", "field_specs", nullptr};
    PyObject* parsed_query;
    PyObject* field_specs;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO", (char**)kwlist,
            &parsed_query, &field_specs)) return nullptr;

    if (!PyDict_Check(parsed_query) || !PyList_Check(field_specs)) {
        PyErr_SetString(PyExc_TypeError, "expected dict and list");
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    Py_ssize_t nspecs = PyList_GET_SIZE(field_specs);
    for (Py_ssize_t i = 0; i < nspecs; i++) {
        PyObject* spec = PyList_GET_ITEM(field_specs, i);
        if (!PyTuple_Check(spec) || PyTuple_GET_SIZE(spec) < 3) continue;

        PyObject* field_name = PyTuple_GET_ITEM(spec, 0);
        PyObject* alias = PyTuple_GET_ITEM(spec, 1);
        // spec[2] = required (bool)

        // Try alias first, then field_name
        PyObject* val = PyDict_GetItem(parsed_query, alias);  // borrowed
        if (!val) val = PyDict_GetItem(parsed_query, field_name);  // borrowed
        if (val) {
            // If list, get first value
            if (PyList_Check(val) && PyList_GET_SIZE(val) > 0) {
                val = PyList_GET_ITEM(val, 0);
            }
            PyDict_SetItem(result.get(), field_name, val);
        }
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_cookie_header(cookie_string: str) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_cookie_header(PyObject* self, PyObject* arg) {
    const char* cookie;
    Py_ssize_t cookie_len;
    if (PyUnicode_Check(arg)) {
        cookie = PyUnicode_AsUTF8AndSize(arg, &cookie_len);
        if (!cookie) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    const char* p = cookie;
    const char* end = cookie + cookie_len;

    while (p < end) {
        // Skip whitespace and semicolons
        while (p < end && (*p == ' ' || *p == ';' || *p == '\t')) p++;
        if (p >= end) break;

        const char* key_start = p;
        while (p < end && *p != '=') p++;
        if (p >= end) break;

        std::string key(key_start, p - key_start);
        p++;  // skip '='

        const char* val_start = p;
        while (p < end && *p != ';') p++;
        std::string val(val_start, p - val_start);

        // Trim trailing whitespace from value
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();

        PyRef py_key(PyUnicode_FromStringAndSize(key.c_str(), key.size()));
        PyRef py_val(PyUnicode_FromStringAndSize(val.c_str(), val.size()));
        if (!py_key || !py_val) return nullptr;
        PyDict_SetItem(result.get(), py_key.get(), py_val.get());
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_content_type(content_type: str) → Tuple[str, str]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_content_type(PyObject* self, PyObject* arg) {
    const char* ct;
    Py_ssize_t ct_len;
    if (PyUnicode_Check(arg)) {
        ct = PyUnicode_AsUTF8AndSize(arg, &ct_len);
        if (!ct) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return nullptr;
    }

    // Find '/' separator, stop at ';' (params)
    const char* end = ct + ct_len;
    const char* slash = nullptr;
    const char* semi = nullptr;
    for (const char* p = ct; p < end; p++) {
        if (*p == '/' && !slash) slash = p;
        if (*p == ';' && !semi) { semi = p; break; }
    }

    if (!slash) {
        // No slash — return (content_type, "")
        PyRef main_type(PyUnicode_FromStringAndSize(ct, semi ? (semi - ct) : ct_len));
        PyRef sub_type(PyUnicode_FromString(""));
        return PyTuple_Pack(2, main_type.get(), sub_type.get());
    }

    const char* type_end = semi ? semi : end;
    PyRef main_type(PyUnicode_FromStringAndSize(ct, slash - ct));
    PyRef sub_type(PyUnicode_FromStringAndSize(slash + 1, type_end - slash - 1));
    if (!main_type || !sub_type) return nullptr;
    return PyTuple_Pack(2, main_type.get(), sub_type.get());
}

// ══════════════════════════════════════════════════════════════════════════════
// is_json_content_type(content_type: str) → bool
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_is_json_content_type(PyObject* self, PyObject* arg) {
    const char* ct;
    Py_ssize_t ct_len;
    if (PyUnicode_Check(arg)) {
        ct = PyUnicode_AsUTF8AndSize(arg, &ct_len);
        if (!ct) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return nullptr;
    }

    // Lowercase and check for "json" substring
    std::string lower(ct, ct_len);
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }

    if (lower.find("json") != std::string::npos ||
        lower == "application/json" ||
        lower.find("application/json") == 0 ||
        lower.find("+json") != std::string::npos) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// ══════════════════════════════════════════════════════════════════════════════
// batch_coerce_scalars(params: PyDict, field_specs: PyList) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

static inline PyObject* coerce_to_int(const char* s, Py_ssize_t len) {
    char* endptr;
    long long v = strtoll(s, &endptr, 10);
    if (endptr == s + len) return PyLong_FromLongLong(v);
    // Fallback: return original string
    return PyUnicode_FromStringAndSize(s, len);
}

static inline PyObject* coerce_to_float(const char* s, Py_ssize_t len) {
    char* endptr;
    double v = strtod(s, &endptr);
    if (endptr == s + len) return PyFloat_FromDouble(v);
    return PyUnicode_FromStringAndSize(s, len);
}

static inline PyObject* coerce_to_bool(const char* s, Py_ssize_t len) {
    if (len == 4 && (memcmp(s, "true", 4) == 0 || memcmp(s, "True", 4) == 0)) Py_RETURN_TRUE;
    if (len == 1 && s[0] == '1') Py_RETURN_TRUE;
    if (len == 3 && (memcmp(s, "yes", 3) == 0 || memcmp(s, "Yes", 3) == 0)) Py_RETURN_TRUE;
    if (len == 2 && (memcmp(s, "on", 2) == 0 || memcmp(s, "On", 2) == 0)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

PyObject* py_batch_coerce_scalars(PyObject* self, PyObject* args) {
    PyObject* params;
    PyObject* field_specs;
    if (!PyArg_ParseTuple(args, "OO", &params, &field_specs)) return nullptr;

    if (!PyDict_Check(params) || !PyList_Check(field_specs)) {
        PyErr_SetString(PyExc_TypeError, "expected dict and list");
        return nullptr;
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    // Copy all items first
    PyObject* key;
    PyObject* value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(params, &pos, &key, &value)) {
        PyDict_SetItem(result.get(), key, value);
    }

    // Apply coercion based on field_specs
    Py_ssize_t nspecs = PyList_GET_SIZE(field_specs);
    for (Py_ssize_t i = 0; i < nspecs; i++) {
        PyObject* spec = PyList_GET_ITEM(field_specs, i);
        if (!PyDict_Check(spec)) continue;

        PyObject* fname = PyDict_GetItemString(spec, "field_name");
        PyObject* type_tag = PyDict_GetItemString(spec, "type_tag");
        if (!fname || !type_tag) continue;

        PyObject* val = PyDict_GetItem(result.get(), fname);  // borrowed
        if (!val || !PyUnicode_Check(val)) continue;

        const char* s;
        Py_ssize_t slen;
        s = PyUnicode_AsUTF8AndSize(val, &slen);
        if (!s) continue;

        int tt = (int)PyLong_AsLong(type_tag);
        PyObject* coerced = nullptr;
        switch (tt) {
            case 1: coerced = coerce_to_int(s, slen); break;     // TYPE_INT
            case 2: coerced = coerce_to_float(s, slen); break;   // TYPE_FLOAT
            case 3: coerced = coerce_to_bool(s, slen); break;    // TYPE_BOOL
            default: continue;
        }
        if (coerced) {
            PyDict_SetItem(result.get(), fname, coerced);
            Py_DECREF(coerced);
        }
    }

    return result.release();
}
