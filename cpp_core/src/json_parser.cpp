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

// ── 2-phase API for GIL release ─────────────────────────────────────────────
// Phase 1: Pure C parse — safe to call without the GIL held.
// Returns yyjson_doc* on success, nullptr on error.
// No Python exceptions set (caller must check and handle).

yyjson_doc* yyjson_parse_raw(const char* data, size_t len) {
    if (!data || len == 0) return nullptr;
    return yyjson_read_opts((char*)data, len, YYJSON_READ_NOFLAG, nullptr, nullptr);
}

yyjson_doc* yyjson_parse_raw_with_err(const char* data, size_t len, const char** err_msg) {
    if (!data || len == 0) {
        if (err_msg) *err_msg = "Empty input";
        return nullptr;
    }
    yyjson_read_err err;
    yyjson_doc* doc = yyjson_read_opts((char*)data, len, YYJSON_READ_NOFLAG, nullptr, &err);
    if (!doc && err_msg) {
        *err_msg = (err.msg && err.msg[0]) ? err.msg : "Invalid JSON";
    }
    return doc;
}

// Phase 2: Convert parsed yyjson_doc to Python objects — requires GIL.
// Frees the doc internally. Returns new reference or NULL with exception set.

PyObject* yyjson_doc_to_pyobject(yyjson_doc* doc) {
    if (!doc) {
        PyErr_SetString(PyExc_ValueError, "NULL yyjson_doc");
        return nullptr;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    PyObject* result = val_to_pyobject(root);
    yyjson_doc_free(doc);
    return result;
}

// ── Phase 2b: Merge yyjson object directly into existing dict ───────────────
// For embed_body_fields: merges top-level JSON object key-value pairs directly
// into target_dict (kwargs), avoiding intermediate dict + PyDict_Update.
// Also produces out_full_dict (the full body dict) for paths that need it
// (model_validate, request_body_to_args, InlineResult).
// Frees doc internally. Returns 0 on success, -1 on error.

int yyjson_doc_merge_to_dict(yyjson_doc* doc, PyObject* target_dict, PyObject** out_full_dict) {
    if (!doc) {
        PyErr_SetString(PyExc_ValueError, "NULL yyjson_doc");
        return -1;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || yyjson_get_type(root) != YYJSON_TYPE_OBJ) {
        // Not a JSON object — fall back to full conversion
        PyObject* result = val_to_pyobject(root);
        yyjson_doc_free(doc);
        if (!result) return -1;
        *out_full_dict = result;
        // Can't merge non-object into dict
        return 0;
    }

    size_t count = yyjson_obj_size(root);
    PyRef full_dict(PyDict_New());
    if (!full_dict) { yyjson_doc_free(doc); return -1; }

    // Single pass: create each key-value pair once, insert into BOTH dicts
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    for (size_t i = 0; i < count; i++) {
        yyjson_val* key = yyjson_obj_iter_next(&iter);
        yyjson_val* value = yyjson_obj_iter_get_val(key);

        const char* ks = yyjson_get_str(key);
        size_t klen = yyjson_get_len(key);
        PyRef py_key(PyUnicode_FromStringAndSize(ks, (Py_ssize_t)klen));
        if (!py_key) { yyjson_doc_free(doc); return -1; }

        PyRef py_val(val_to_pyobject(value));
        if (!py_val) { yyjson_doc_free(doc); return -1; }

        // Insert into kwargs (target) — the embed merge
        if (PyDict_SetItem(target_dict, py_key.get(), py_val.get()) < 0) {
            yyjson_doc_free(doc);
            return -1;
        }
        // Insert into full_dict — for later use by model_validate etc.
        if (PyDict_SetItem(full_dict.get(), py_key.get(), py_val.get()) < 0) {
            yyjson_doc_free(doc);
            return -1;
        }
    }

    yyjson_doc_free(doc);
    *out_full_dict = full_dict.release();
    return 0;
}
