#include "json_parser.hpp"
#include "pyref.hpp"

// yyjson is C — extern "C" guard in its header handles this
#include "yyjson/yyjson.h"

// ── Recursive converter: yyjson_val* → PyObject* (new reference) ────────────

static PyObject* val_to_pyobject(yyjson_val* val) {
    if (!val) Py_RETURN_NONE;

    yyjson_type type = yyjson_get_type(val);

    switch (type) {
        case YYJSON_TYPE_NULL:
            Py_RETURN_NONE;

        case YYJSON_TYPE_BOOL:
            if (yyjson_get_bool(val)) Py_RETURN_TRUE;
            Py_RETURN_FALSE;

        case YYJSON_TYPE_NUM: {
            yyjson_subtype subtype = yyjson_get_subtype(val);
            if (subtype == YYJSON_SUBTYPE_UINT)
                return PyLong_FromUnsignedLongLong(yyjson_get_uint(val));
            if (subtype == YYJSON_SUBTYPE_SINT)
                return PyLong_FromLongLong(yyjson_get_sint(val));
            // YYJSON_SUBTYPE_REAL
            return PyFloat_FromDouble(yyjson_get_real(val));
        }

        case YYJSON_TYPE_STR: {
            const char* s = yyjson_get_str(val);
            size_t slen = yyjson_get_len(val);
            return PyUnicode_FromStringAndSize(s, (Py_ssize_t)slen);
        }

        case YYJSON_TYPE_ARR: {
            size_t count = yyjson_arr_size(val);
            PyRef list(PyList_New((Py_ssize_t)count));
            if (!list) return nullptr;

            yyjson_arr_iter iter;
            yyjson_arr_iter_init(val, &iter);
            for (size_t i = 0; i < count; i++) {
                yyjson_val* elem = yyjson_arr_iter_next(&iter);
                PyObject* py_elem = val_to_pyobject(elem);
                if (!py_elem) return nullptr;
                PyList_SET_ITEM(list.get(), (Py_ssize_t)i, py_elem);  // steals ref
            }
            return list.release();
        }

        case YYJSON_TYPE_OBJ: {
            size_t count = yyjson_obj_size(val);
            PyRef dict(PyDict_New());
            if (!dict) return nullptr;

            yyjson_obj_iter iter;
            yyjson_obj_iter_init(val, &iter);
            for (size_t i = 0; i < count; i++) {
                yyjson_val* key = yyjson_obj_iter_next(&iter);
                yyjson_val* value = yyjson_obj_iter_get_val(key);

                const char* ks = yyjson_get_str(key);
                size_t klen = yyjson_get_len(key);
                PyRef py_key(PyUnicode_FromStringAndSize(ks, (Py_ssize_t)klen));
                if (!py_key) return nullptr;

                PyRef py_val(val_to_pyobject(value));
                if (!py_val) return nullptr;

                if (PyDict_SetItem(dict.get(), py_key.get(), py_val.get()) < 0)
                    return nullptr;
            }
            return dict.release();
        }

        default:
            Py_RETURN_NONE;
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

PyObject* yyjson_parse_to_pyobject(const char* data, size_t len) {
    if (!data || len == 0) {
        PyErr_SetString(PyExc_ValueError, "Empty JSON input");
        return nullptr;
    }

    yyjson_read_err err;
    yyjson_doc* doc = yyjson_read_opts(
        (char*)data, len, YYJSON_READ_NOFLAG, nullptr, &err);

    if (!doc) {
        PyErr_Format(PyExc_ValueError,
            "JSON parse error at position %zu: %s",
            err.pos, err.msg ? err.msg : "unknown error");
        return nullptr;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    PyObject* result = val_to_pyobject(root);
    yyjson_doc_free(doc);  // Always free — data is now in Python objects
    return result;
}
