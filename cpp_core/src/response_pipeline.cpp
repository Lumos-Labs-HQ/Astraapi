#define PY_SSIZE_T_CLEAN
#include "json_writer.hpp"
#include "pyref.hpp"

#include <Python.h>

extern PyObject *py_fast_jsonable_encode(PyObject *self, PyObject *arg);

// encode_to_json_bytes(obj: PyAny) → PyBytes
//
// Combined encode + serialize: first run fast_jsonable_encode to make the
// object JSON-serializable, then serialize to bytes via streaming writer.

PyObject *py_encode_to_json_bytes(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *kwlist[] = {"obj", nullptr};
    PyObject *obj;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", (char **)kwlist, &obj)) {
        return nullptr;
    }

    PyRef encoded(py_fast_jsonable_encode(self, obj));
    if (!encoded) return nullptr;

    return serialize_to_json_pybytes(encoded.get());
}

// encode_to_json_bytes_pretty(obj: PyAny) → PyBytes

PyObject *py_encode_to_json_bytes_pretty(PyObject *self, PyObject *args, PyObject *kwargs) {
    static const char *kwlist[] = {"obj", nullptr};
    PyObject *obj;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", (char **)kwlist, &obj)) {
        return nullptr;
    }

    PyRef encoded(py_fast_jsonable_encode(self, obj));
    if (!encoded) return nullptr;

    static PyObject *json_dumps = nullptr;
    if (!json_dumps) {
        PyRef json_mod(PyImport_ImportModule("json"));
        if (!json_mod) return nullptr;
        json_dumps = PyObject_GetAttrString(json_mod.get(), "dumps");
        if (!json_dumps) return nullptr;
    }

    PyRef call_args(PyTuple_Pack(1, encoded.get()));
    PyRef call_kwargs(PyDict_New());
    if (!call_args || !call_kwargs) return nullptr;

    PyRef indent(PyLong_FromLong(2));
    PyDict_SetItemString(call_kwargs.get(), "indent", indent.get());
    PyDict_SetItemString(call_kwargs.get(), "ensure_ascii", Py_False);

    PyRef result(PyObject_Call(json_dumps, call_args.get(), call_kwargs.get()));
    if (!result) return nullptr;

    // Convert string to bytes
    if (PyUnicode_Check(result.get())) {
        return PyUnicode_AsEncodedString(result.get(), "utf-8", "strict");
    }

    Py_INCREF(result.get());
    return result.release();
}
