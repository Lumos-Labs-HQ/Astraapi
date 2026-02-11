#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_writer.hpp"
#include "json_parser.hpp"
#include "pyref.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// parse_json_body(data: bytes) → PyAny — yyjson (~3GB/s)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_json_body(PyObject* self, PyObject* arg) {
    if (!PyBytes_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected bytes");
        return nullptr;
    }

    char* data;
    Py_ssize_t data_len;
    if (PyBytes_AsStringAndSize(arg, &data, &data_len) < 0)
        return nullptr;

    if (data_len == 0) {
        PyErr_SetString(PyExc_ValueError, "Empty JSON input");
        return nullptr;
    }

    return yyjson_parse_to_pyobject(data, (size_t)data_len);
}

// ══════════════════════════════════════════════════════════════════════════════
// serialize_to_json_bytes(obj: PyAny) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_serialize_to_json_bytes(PyObject* self, PyObject* arg) {
    return serialize_to_json_pybytes(arg);
}

// ══════════════════════════════════════════════════════════════════════════════
// serialize_to_json_bytes_pretty(obj: PyAny) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_serialize_to_json_bytes_pretty(PyObject* self, PyObject* arg) {
    // Use Python's json.dumps for pretty printing
    static PyObject* json_dumps = nullptr;
    if (!json_dumps) {
        PyRef json_mod(PyImport_ImportModule("json"));
        if (!json_mod) return nullptr;
        json_dumps = PyObject_GetAttrString(json_mod.get(), "dumps");
        if (!json_dumps) return nullptr;
    }

    // json.dumps(obj, indent=2, ensure_ascii=False)
    PyRef py_args(PyTuple_Pack(1, arg));
    PyRef py_kwargs(PyDict_New());
    if (!py_args || !py_kwargs) return nullptr;

    PyRef indent(PyLong_FromLong(2));
    PyDict_SetItemString(py_kwargs.get(), "indent", indent.get());
    PyDict_SetItemString(py_kwargs.get(), "ensure_ascii", Py_False);

    PyRef result(PyObject_Call(json_dumps, py_args.get(), py_kwargs.get()));
    if (!result) return nullptr;

    // Convert string to bytes
    if (PyUnicode_Check(result.get())) {
        return PyUnicode_AsEncodedString(result.get(), "utf-8", "strict");
    }

    Py_INCREF(result.get());
    return result.release();
}
