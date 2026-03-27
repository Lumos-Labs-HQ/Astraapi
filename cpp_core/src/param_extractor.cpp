#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <cstring>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Inline param extraction — no global registry, no mutex.
// Called from Python's solve_dependencies() with pre-built field_specs list.
// ══════════════════════════════════════════════════════════════════════════════

struct ParamSpec {
    const char* field_name;   // points into Python unicode object (stable)
    const char* alias;
    const char* header_lookup_key;
    int location;   // 0=query, 1=header, 2=cookie, 3=path
    int type_tag;   // 0=str, 1=int, 2=float, 3=bool
    bool required;
    bool is_sequence;
    PyObject* default_value;  // borrowed ref into the Python spec dict
};

// ══════════════════════════════════════════════════════════════════════════════
// Scalar coercion helper — converts str to int/float/bool where possible
// ══════════════════════════════════════════════════════════════════════════════

static PyObject* coerce_value(PyObject* val, int type_tag) {
    if (!PyUnicode_Check(val)) {
        Py_INCREF(val);
        return val;
    }

    const char* s;
    Py_ssize_t slen;
    s = PyUnicode_AsUTF8AndSize(val, &slen);
    if (!s) { Py_INCREF(val); return val; }

    switch (type_tag) {
        case 1: {  // int
            char* endptr;
            long long v = strtoll(s, &endptr, 10);
            if (endptr == s + slen) return PyLong_FromLongLong(v);
            Py_INCREF(val);
            return val;
        }
        case 2: {  // float
            char* endptr;
            double v = strtod(s, &endptr);
            if (endptr == s + slen) return PyFloat_FromDouble(v);
            Py_INCREF(val);
            return val;
        }
        case 3: {  // bool — only accept standard bool strings
            if ((slen == 4 && (memcmp(s, "true", 4) == 0 || memcmp(s, "True", 4) == 0)) ||
                (slen == 1 && s[0] == '1') ||
                (slen == 3 && (memcmp(s, "yes", 3) == 0 || memcmp(s, "Yes", 3) == 0)) ||
                (slen == 2 && (memcmp(s, "on", 2) == 0 || memcmp(s, "On", 2) == 0))) {
                Py_RETURN_TRUE;
            }
            if ((slen == 5 && (memcmp(s, "false", 5) == 0 || memcmp(s, "False", 5) == 0)) ||
                (slen == 1 && s[0] == '0') ||
                (slen == 2 && (memcmp(s, "no", 2) == 0 || memcmp(s, "No", 2) == 0)) ||
                (slen == 3 && (memcmp(s, "off", 3) == 0 || memcmp(s, "Off", 3) == 0))) {
                Py_RETURN_FALSE;
            }
            Py_INCREF(val);
            return val;
        }
        default:
            Py_INCREF(val);
            return val;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// extract_from_source — inner loop for one location (query/header/cookie/path)
// Uses PyUnicode_InternFromString so dict key lookups use pointer equality.
// ══════════════════════════════════════════════════════════════════════════════

static void extract_from_source(PyObject* result, PyObject* source,
                                 const std::vector<ParamSpec>& specs, int location) {
    if (!source || source == Py_None || !PyDict_Check(source)) return;

    for (const auto& ps : specs) {
        if (ps.location != location) continue;

        const char* lookup_key = ps.alias ? ps.alias : ps.field_name;
        if (location == 1 && ps.header_lookup_key && ps.header_lookup_key[0]) {
            lookup_key = ps.header_lookup_key;
        }

        // InternFromString: returns the same interned object on every call → pointer-equal
        // dict key lookup uses pointer compare (no hash computation for interned strings)
        PyObject* py_lookup = PyUnicode_InternFromString(lookup_key);
        if (!py_lookup) continue;
        PyObject* val = PyDict_GetItem(source, py_lookup);  // borrowed
        Py_DECREF(py_lookup);

        PyObject* py_fname = PyUnicode_InternFromString(ps.field_name);
        if (!py_fname) continue;

        if (val) {
            if (PyList_Check(val)) {
                if (ps.is_sequence) {
                    PyDict_SetItem(result, py_fname, val);
                } else if (PyList_GET_SIZE(val) > 0) {
                    PyObject* coerced = coerce_value(PyList_GET_ITEM(val, 0), ps.type_tag);
                    if (coerced) {
                        PyDict_SetItem(result, py_fname, coerced);
                        Py_DECREF(coerced);
                    }
                }
            } else {
                PyObject* coerced = coerce_value(val, ps.type_tag);
                if (coerced) {
                    PyDict_SetItem(result, py_fname, coerced);
                    Py_DECREF(coerced);
                }
            }
        } else if (ps.default_value) {
            PyDict_SetItem(result, py_fname, ps.default_value);
        }
        Py_DECREF(py_fname);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// batch_extract_params_inline(query_params, headers, cookies, path_params, field_specs)
// All param sources + specs passed directly — zero global state, zero mutex.
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_batch_extract_params_inline(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "query_params", "headers", "cookies", "path_params", "field_specs", nullptr
    };

    PyObject* query_params;
    PyObject* headers;
    PyObject* cookies;
    PyObject* path_params;
    PyObject* field_specs_list;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOOOO", (char**)kwlist,
            &query_params, &headers, &cookies, &path_params, &field_specs_list)) {
        return nullptr;
    }

    if (!PyList_Check(field_specs_list)) {
        PyErr_SetString(PyExc_TypeError, "field_specs must be a list");
        return nullptr;
    }

    // Build temporary specs from Python dicts. field_name/alias pointers are
    // into interned Python unicode objects so they remain valid for the call duration.
    std::vector<ParamSpec> specs;
    Py_ssize_t n = PyList_GET_SIZE(field_specs_list);
    specs.reserve((size_t)n);

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* sd = PyList_GET_ITEM(field_specs_list, i);
        if (!PyDict_Check(sd)) continue;

        ParamSpec ps;
        PyObject* fn  = PyDict_GetItemString(sd, "field_name");
        PyObject* al  = PyDict_GetItemString(sd, "alias");
        PyObject* hlk = PyDict_GetItemString(sd, "header_lookup_key");
        PyObject* loc = PyDict_GetItemString(sd, "location");
        PyObject* tt  = PyDict_GetItemString(sd, "type_tag");
        PyObject* def = PyDict_GetItemString(sd, "default_value");
        PyObject* seq = PyDict_GetItemString(sd, "is_sequence");

        ps.field_name        = (fn  && PyUnicode_Check(fn))  ? PyUnicode_AsUTF8(fn)  : nullptr;
        ps.alias             = (al  && PyUnicode_Check(al))  ? PyUnicode_AsUTF8(al)  : nullptr;
        ps.header_lookup_key = (hlk && PyUnicode_Check(hlk)) ? PyUnicode_AsUTF8(hlk) : nullptr;
        ps.location          = loc ? (int)PyLong_AsLong(loc) : 0;
        ps.type_tag          = tt  ? (int)PyLong_AsLong(tt)  : 0;
        ps.is_sequence       = seq ? (PyObject_IsTrue(seq) == 1) : false;
        ps.default_value     = (def && def != Py_None) ? def : nullptr;

        if (!ps.field_name) continue;
        specs.push_back(ps);
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    extract_from_source(result.get(), query_params, specs, 0);  // query
    extract_from_source(result.get(), headers,      specs, 1);  // header
    extract_from_source(result.get(), cookies,      specs, 2);  // cookie
    extract_from_source(result.get(), path_params,  specs, 3);  // path

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// cleanup_param_registry — no-op now (no global registry), kept for link compat
// ══════════════════════════════════════════════════════════════════════════════

void cleanup_param_registry() {
    // Nothing to clean: global registry removed. PyUnicode_InternFromString
    // interned strings are owned by the interpreter and freed at shutdown.
}
