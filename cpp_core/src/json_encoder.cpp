#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include "json_writer.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// fast_jsonable_encode(obj: PyAny) → PyAny
//
// Pydantic-aware encoder: converts Python objects to JSON-serializable form.
// Handles: BaseModel, dict, list, tuple, datetime, UUID, Enum, Decimal,
//          set, frozenset, bytes, PurePath, GeneratorType.
//
// Returns a JSON-serializable Python object (not bytes).
// ══════════════════════════════════════════════════════════════════════════════

// Cached type objects for isinstance checks
static PyObject* g_datetime_type = nullptr;
static PyObject* g_date_type = nullptr;
static PyObject* g_time_type = nullptr;
static PyObject* g_timedelta_type = nullptr;
static PyObject* g_uuid_type = nullptr;
static PyObject* g_decimal_type = nullptr;
static PyObject* g_enum_type = nullptr;
static PyObject* g_purepath_type = nullptr;
static bool g_types_initialized = false;

static void ensure_types_initialized() {
    if (g_types_initialized) return;

    // datetime types
    PyRef dt_mod(PyImport_ImportModule("datetime"));
    if (dt_mod) {
        g_datetime_type = PyObject_GetAttrString(dt_mod.get(), "datetime");
        g_date_type = PyObject_GetAttrString(dt_mod.get(), "date");
        g_time_type = PyObject_GetAttrString(dt_mod.get(), "time");
        g_timedelta_type = PyObject_GetAttrString(dt_mod.get(), "timedelta");
    }
    PyErr_Clear();

    // UUID
    PyRef uuid_mod(PyImport_ImportModule("uuid"));
    if (uuid_mod) g_uuid_type = PyObject_GetAttrString(uuid_mod.get(), "UUID");
    PyErr_Clear();

    // Decimal
    PyRef decimal_mod(PyImport_ImportModule("decimal"));
    if (decimal_mod) g_decimal_type = PyObject_GetAttrString(decimal_mod.get(), "Decimal");
    PyErr_Clear();

    // Enum
    PyRef enum_mod(PyImport_ImportModule("enum"));
    if (enum_mod) g_enum_type = PyObject_GetAttrString(enum_mod.get(), "Enum");
    PyErr_Clear();

    // PurePath
    PyRef pathlib_mod(PyImport_ImportModule("pathlib"));
    if (pathlib_mod) g_purepath_type = PyObject_GetAttrString(pathlib_mod.get(), "PurePath");
    PyErr_Clear();

    g_types_initialized = true;
}

static PyObject* encode_recursive(PyObject* obj, int depth);

static PyObject* encode_recursive(PyObject* obj, int depth) {
    if (depth > 64) {
        PyErr_SetString(PyExc_ValueError, "Maximum recursion depth exceeded");
        return nullptr;
    }

    // None, bool, int, float, str — return as-is
    if (obj == Py_None || PyBool_Check(obj) || PyLong_Check(obj) ||
        PyFloat_Check(obj) || PyUnicode_Check(obj)) {
        Py_INCREF(obj);
        return obj;
    }

    // dict
    if (PyDict_Check(obj)) {
        PyRef result(PyDict_New());
        if (!result) return nullptr;

        PyObject* key;
        PyObject* value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            PyRef encoded_key(encode_recursive(key, depth + 1));
            PyRef encoded_val(encode_recursive(value, depth + 1));
            if (!encoded_key || !encoded_val) return nullptr;
            // Ensure key is string
            if (!PyUnicode_Check(encoded_key.get())) {
                encoded_key = PyRef(PyObject_Str(encoded_key.get()));
                if (!encoded_key) return nullptr;
            }
            PyDict_SetItem(result.get(), encoded_key.get(), encoded_val.get());
        }
        return result.release();
    }

    // list
    if (PyList_Check(obj)) {
        Py_ssize_t n = PyList_GET_SIZE(obj);
        PyRef result(PyList_New(n));
        if (!result) return nullptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = encode_recursive(PyList_GET_ITEM(obj, i), depth + 1);
            if (!item) return nullptr;
            PyList_SET_ITEM(result.get(), i, item);  // steals ref
        }
        return result.release();
    }

    // tuple
    if (PyTuple_Check(obj)) {
        Py_ssize_t n = PyTuple_GET_SIZE(obj);
        PyRef result(PyList_New(n));
        if (!result) return nullptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = encode_recursive(PyTuple_GET_ITEM(obj, i), depth + 1);
            if (!item) return nullptr;
            PyList_SET_ITEM(result.get(), i, item);
        }
        return result.release();
    }

    // set, frozenset → list
    if (PySet_Check(obj) || PyFrozenSet_Check(obj)) {
        PyRef iter(PyObject_GetIter(obj));
        if (!iter) return nullptr;
        PyRef result(PyList_New(0));
        if (!result) return nullptr;
        while (PyObject* item = PyIter_Next(iter.get())) {
            PyRef encoded(encode_recursive(item, depth + 1));
            Py_DECREF(item);
            if (!encoded) return nullptr;
            PyList_Append(result.get(), encoded.get());
        }
        if (PyErr_Occurred()) return nullptr;
        return result.release();
    }

    // bytes → str (utf-8 decode)
    if (PyBytes_Check(obj)) {
        char* data;
        Py_ssize_t len;
        PyBytes_AsStringAndSize(obj, &data, &len);
        return PyUnicode_FromStringAndSize(data, len);
    }

    ensure_types_initialized();

    // Pydantic BaseModel — try model_dump()
    if (PyObject_HasAttrString(obj, "model_dump")) {
        PyRef dumped(PyObject_CallMethod(obj, "model_dump", nullptr));
        if (dumped) return encode_recursive(dumped.get(), depth + 1);
        PyErr_Clear();
    }

    // Pydantic v1 — try dict()
    if (PyObject_HasAttrString(obj, "dict") && PyObject_HasAttrString(obj, "__fields__")) {
        PyRef dumped(PyObject_CallMethod(obj, "dict", nullptr));
        if (dumped) return encode_recursive(dumped.get(), depth + 1);
        PyErr_Clear();
    }

    // Enum — get .value
    if (g_enum_type && PyObject_IsInstance(obj, g_enum_type)) {
        PyRef val(PyObject_GetAttrString(obj, "value"));
        if (val) return encode_recursive(val.get(), depth + 1);
        PyErr_Clear();
    }

    // datetime, date, time → .isoformat()
    if ((g_datetime_type && PyObject_IsInstance(obj, g_datetime_type)) ||
        (g_date_type && PyObject_IsInstance(obj, g_date_type)) ||
        (g_time_type && PyObject_IsInstance(obj, g_time_type))) {
        return PyObject_CallMethod(obj, "isoformat", nullptr);
    }

    // timedelta → total_seconds()
    if (g_timedelta_type && PyObject_IsInstance(obj, g_timedelta_type)) {
        return PyObject_CallMethod(obj, "total_seconds", nullptr);
    }

    // UUID → str(uuid)
    if (g_uuid_type && PyObject_IsInstance(obj, g_uuid_type)) {
        return PyObject_Str(obj);
    }

    // Decimal → float
    if (g_decimal_type && PyObject_IsInstance(obj, g_decimal_type)) {
        return PyFloat_FromDouble(PyFloat_AsDouble(obj));
    }

    // PurePath → str
    if (g_purepath_type && PyObject_IsInstance(obj, g_purepath_type)) {
        return PyObject_Str(obj);
    }

    // Generator/Iterator → list
    if (PyIter_Check(obj)) {
        PyRef result(PyList_New(0));
        if (!result) return nullptr;
        PyRef iter(PyObject_GetIter(obj));
        if (!iter) return nullptr;
        while (PyObject* item = PyIter_Next(iter.get())) {
            PyRef encoded(encode_recursive(item, depth + 1));
            Py_DECREF(item);
            if (!encoded) return nullptr;
            PyList_Append(result.get(), encoded.get());
        }
        if (PyErr_Occurred()) return nullptr;
        return result.release();
    }

    // Fallback: try __str__
    return PyObject_Str(obj);
}

PyObject* py_fast_jsonable_encode(PyObject* self, PyObject* arg) {
    return encode_recursive(arg, 0);
}
