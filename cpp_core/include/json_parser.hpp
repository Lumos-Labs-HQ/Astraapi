#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>

// Parse JSON bytes to PyObject* using yyjson (~3GB/s).
// Returns new reference to PyDict/PyList/PyUnicode/PyLong/PyFloat/Py_None/Py_True/Py_False.
// Returns NULL with Python exception on error.
// Caller does NOT need to free anything — yyjson_doc is freed internally.
PyObject* yyjson_parse_to_pyobject(const char* data, size_t len);
