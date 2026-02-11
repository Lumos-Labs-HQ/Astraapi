#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_writer.hpp"
#include "buffer_pool.hpp"
#include "pyref.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// serialize_error_response(errors: PyList) → PyBytes
//
// Produces: {"detail": [error_dicts...]} as JSON bytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_serialize_error_response(PyObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list of errors");
        return nullptr;
    }

    // Build {"detail": errors} wrapper dict
    PyRef wrapper(PyDict_New());
    if (!wrapper) return nullptr;

    PyRef detail_key(PyUnicode_InternFromString("detail"));
    PyDict_SetItem(wrapper.get(), detail_key.get(), arg);

    // Serialize to JSON bytes using streaming writer
    return serialize_to_json_pybytes(wrapper.get());
}

// ══════════════════════════════════════════════════════════════════════════════
// serialize_error_list(errors: PyList) → PyBytes
//
// Produces: [error_dicts...] as JSON bytes (no wrapper)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_serialize_error_list(PyObject* self, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list of errors");
        return nullptr;
    }

    return serialize_to_json_pybytes(arg);
}
