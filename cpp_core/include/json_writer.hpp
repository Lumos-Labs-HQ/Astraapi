#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <vector>

// Streaming JSON writer: PyObject* → bytes buffer.
// No intermediate DOM — writes directly to buffer.
// Uses PyDict_Next() for fastest dict iteration and PyList_GET_ITEM() for unchecked list access.

// Write a Python object as JSON into the buffer. Returns 0 on success, -1 on error.
int write_json(PyObject* obj, std::vector<char>& buf, int depth);

// Serialize a Python object to JSON bytes (PyBytes), using the buffer pool.
// Returns a new reference to PyBytes, or NULL on error.
PyObject* serialize_to_json_pybytes(PyObject* obj);

// Clean up cached type objects (call at module shutdown)
void json_writer_cleanup();

// Fast i64 to buffer (itoa replacement)
int fast_i64_to_buf(char* buf, long long val);
