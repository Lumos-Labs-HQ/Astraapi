#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>

// Forward declare yyjson types to avoid exposing the full header
struct yyjson_doc;

// Parse JSON bytes to PyObject* using yyjson (~3GB/s).
// Returns new reference to PyDict/PyList/PyUnicode/PyLong/PyFloat/Py_None/Py_True/Py_False.
// Returns NULL with Python exception on error.
// Caller does NOT need to free anything — yyjson_doc is freed internally.
PyObject* yyjson_parse_to_pyobject(const char* data, size_t len);

// ── 2-phase API for GIL release ──────────────────────────────────────────────
// Phase 1: Pure C parse (safe to call without GIL).
// Returns yyjson_doc* on success, nullptr on error (no Python exception set).
yyjson_doc* yyjson_parse_raw(const char* data, size_t len);

// Phase 2: Convert yyjson_doc to Python objects (requires GIL).
// Frees the doc internally. Returns new reference or NULL with exception set.
PyObject* yyjson_doc_to_pyobject(yyjson_doc* doc);

// Phase 2b: Merge yyjson object keys directly into an existing PyDict (requires GIL).
// For top-level JSON objects only — merges key-value pairs into target_dict.
// Avoids creating an intermediate Python dict + PyDict_Update.
// Frees the doc internally. Returns 0 on success, -1 on error with exception set.
// Also sets *out_full_dict to a new Python dict of the full body (for later paths that need it).
int yyjson_doc_merge_to_dict(yyjson_doc* doc, PyObject* target_dict, PyObject** out_full_dict);
