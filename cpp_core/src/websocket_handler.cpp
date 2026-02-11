#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_parser.hpp"
#include "json_writer.hpp"
#include "pyref.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// ws_parse_json(data: bytes|str) → PyAny  (yyjson — zero Python calls)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_parse_json(PyObject* self, PyObject* arg) {
    const char* data = nullptr;
    Py_ssize_t len = 0;

    if (PyBytes_Check(arg)) {
        data = PyBytes_AS_STRING(arg);
        len = PyBytes_GET_SIZE(arg);
    } else if (PyUnicode_Check(arg)) {
        data = PyUnicode_AsUTF8AndSize(arg, &len);
        if (!data) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected bytes or str");
        return nullptr;
    }

    if (len <= 0) {
        PyErr_SetString(PyExc_ValueError, "Empty JSON input");
        return nullptr;
    }

    return yyjson_parse_to_pyobject(data, static_cast<size_t>(len));
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_serialize_json(obj: PyAny) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_serialize_json(PyObject* self, PyObject* arg) {
    return serialize_to_json_pybytes(arg);
}

// ══════════════════════════════════════════════════════════════════════════════
// ws_batch_parse(messages: PyList) → PyList
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_ws_batch_parse(PyObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list");
        return nullptr;
    }

    Py_ssize_t n = PyList_GET_SIZE(arg);
    PyRef result(PyList_New(n));
    if (!result) return nullptr;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* msg = PyList_GET_ITEM(arg, i);  // borrowed
        const char* data = nullptr;
        Py_ssize_t len = 0;
        PyObject* parsed = nullptr;

        if (PyBytes_Check(msg)) {
            data = PyBytes_AS_STRING(msg);
            len = PyBytes_GET_SIZE(msg);
        } else if (PyUnicode_Check(msg)) {
            data = PyUnicode_AsUTF8AndSize(msg, &len);
        }

        if (data && len > 0) {
            parsed = yyjson_parse_to_pyobject(data, static_cast<size_t>(len));
        }

        if (!parsed) {
            // On parse error, keep original
            PyErr_Clear();
            Py_INCREF(msg);
            parsed = msg;
        }
        PyList_SET_ITEM(result.get(), i, parsed);  // steals ref
    }

    return result.release();
}
