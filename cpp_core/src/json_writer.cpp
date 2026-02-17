#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_writer.hpp"
#include "asgi_constants.hpp"
#include "buffer_pool.hpp"
#include "pyref.hpp"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <mutex>

extern "C" {
#include "ryu/ryu.h"
}

// ── Cached type objects for special serialization ─────────────────────────────
static PyObject* s_datetime_type = nullptr;    // datetime.datetime
static PyObject* s_date_type = nullptr;        // datetime.date
static PyObject* s_time_type = nullptr;        // datetime.time
static PyObject* s_decimal_type = nullptr;     // decimal.Decimal
static PyObject* s_uuid_type = nullptr;        // uuid.UUID
static PyObject* s_enum_type = nullptr;        // enum.Enum
static PyObject* s_isoformat = nullptr;        // cached "isoformat" string
static PyObject* s_value = nullptr;            // cached "value" string

// TS-1: Thread-safe lazy init for free-threaded Python (PEP 703) readiness
static std::once_flag s_types_init_flag;

static void _do_ensure_special_types() {

    s_isoformat = PyUnicode_InternFromString("isoformat");
    s_value = PyUnicode_InternFromString("value");

    // datetime module
    PyRef dt_mod(PyImport_ImportModule("datetime"));
    if (dt_mod) {
        s_datetime_type = PyObject_GetAttrString(dt_mod.get(), "datetime");
        s_date_type = PyObject_GetAttrString(dt_mod.get(), "date");
        s_time_type = PyObject_GetAttrString(dt_mod.get(), "time");
    }
    PyErr_Clear();

    // decimal module
    PyRef dec_mod(PyImport_ImportModule("decimal"));
    if (dec_mod) {
        s_decimal_type = PyObject_GetAttrString(dec_mod.get(), "Decimal");
    }
    PyErr_Clear();

    // uuid module
    PyRef uuid_mod(PyImport_ImportModule("uuid"));
    if (uuid_mod) {
        s_uuid_type = PyObject_GetAttrString(uuid_mod.get(), "UUID");
    }
    PyErr_Clear();

    // enum module
    PyRef enum_mod(PyImport_ImportModule("enum"));
    if (enum_mod) {
        s_enum_type = PyObject_GetAttrString(enum_mod.get(), "Enum");
    }
    PyErr_Clear();
}

static void ensure_special_types() {
    std::call_once(s_types_init_flag, _do_ensure_special_types);
}

void json_writer_init() {
    ensure_special_types();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static inline void buf_append(std::vector<char>& buf, const char* s, size_t len) {
    buf.insert(buf.end(), s, s + len);
}

static inline void buf_push(std::vector<char>& buf, char c) {
    buf.push_back(c);
}

// Batch-scan string escaping: copy safe ranges in one memcpy, only branch on escapable chars.
// Most strings are pure ASCII with no escapes — this is ~3-5x faster than per-character switch.
static void write_escaped_string(std::vector<char>& buf, const char* s, Py_ssize_t len) {
    buf.push_back('"');
    const char* p = s;
    const char* end = s + len;
    const char* safe = p;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        // Pass through: printable ASCII (except " and \) AND all UTF-8 bytes (>= 0x80).
        // UTF-8 continuation/lead bytes must NOT be escaped individually — doing so
        // would produce invalid JSON like "\u00f0\u009f" instead of the original UTF-8.
        if ((c >= 0x20 && c != '"' && c != '\\') || c >= 0x80) { p++; continue; }
        // Flush safe range
        if (p > safe) buf.insert(buf.end(), safe, p);
        // Emit escape
        switch (c) {
            case '"':  { static const char e[]="\\\""; buf.insert(buf.end(),e,e+2); break; }
            case '\\': { static const char e[]="\\\\"; buf.insert(buf.end(),e,e+2); break; }
            case '\b': { static const char e[]="\\b";  buf.insert(buf.end(),e,e+2); break; }
            case '\f': { static const char e[]="\\f";  buf.insert(buf.end(),e,e+2); break; }
            case '\n': { static const char e[]="\\n";  buf.insert(buf.end(),e,e+2); break; }
            case '\r': { static const char e[]="\\r";  buf.insert(buf.end(),e,e+2); break; }
            case '\t': { static const char e[]="\\t";  buf.insert(buf.end(),e,e+2); break; }
            default: {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                buf.insert(buf.end(), esc, esc + 6);
                break;
            }
        }
        safe = ++p;
    }
    if (p > safe) buf.insert(buf.end(), safe, p);
    buf.push_back('"');
}

// ── Main writer ─────────────────────────────────────────────────────────────

int write_json(PyObject* obj, std::vector<char>& buf, int depth) {
    if (depth > 64) {
        PyErr_SetString(PyExc_ValueError, "JSON nesting too deep (>64)");
        return -1;
    }

    // None → null
    if (obj == Py_None) {
        buf_append(buf, "null", 4);
        return 0;
    }

    // Bool (check before int — bool is subclass of int)
    if (PyBool_Check(obj)) {
        if (obj == Py_True) {
            buf_append(buf, "true", 4);
        } else {
            buf_append(buf, "false", 5);
        }
        return 0;
    }

    // Int
    if (PyLong_Check(obj)) {
        int overflow = 0;
        long long val = PyLong_AsLongLongAndOverflow(obj, &overflow);
        if (overflow == 0 && !PyErr_Occurred()) {
            char num_buf[32];
            int n = fast_i64_to_buf(num_buf, val);
            buf_append(buf, num_buf, (size_t)n);
            return 0;
        }
        PyErr_Clear();
        // Fallback: convert to string
        PyObject* str = PyObject_Str(obj);
        if (!str) return -1;
        Py_ssize_t slen;
        const char* s = PyUnicode_AsUTF8AndSize(str, &slen);
        if (!s) { Py_DECREF(str); return -1; }
        buf_append(buf, s, (size_t)slen);
        Py_DECREF(str);
        return 0;
    }

    // Float (ryu — shortest exact representation, ~10x faster than snprintf)
    if (PyFloat_Check(obj)) {
        double val = PyFloat_AS_DOUBLE(obj);
        // Handle special values
        if (std::isnan(val)) {
            PyErr_SetString(PyExc_ValueError, "NaN is not JSON serializable");
            return -1;
        }
        if (std::isinf(val)) {
            PyErr_SetString(PyExc_ValueError, "Infinity is not JSON serializable");
            return -1;
        }
        char num_buf[32];
        int n = d2s_buffered_n(val, num_buf);
        // ryu uses 'E' for exponent — JSON convention is lowercase 'e'
        for (int i = 0; i < n; i++) {
            if (num_buf[i] == 'E') { num_buf[i] = 'e'; break; }
        }
        buf_append(buf, num_buf, (size_t)n);
        return 0;
    }

    // String
    if (PyUnicode_Check(obj)) {
        Py_ssize_t slen;
        const char* s = PyUnicode_AsUTF8AndSize(obj, &slen);
        if (!s) return -1;
        write_escaped_string(buf, s, slen);
        return 0;
    }

    // Bytes → base64 or raw string
    if (PyBytes_Check(obj)) {
        Py_ssize_t blen;
        char* bdata;
        PyBytes_AsStringAndSize(obj, &bdata, &blen);
        write_escaped_string(buf, bdata, blen);
        return 0;
    }

    // Dict
    if (PyDict_Check(obj)) {
        Py_ssize_t dict_size = PyDict_GET_SIZE(obj);
        if (dict_size > 4) buf.reserve(buf.size() + dict_size * 48);
        buf_push(buf, '{');
        PyObject* key;
        PyObject* value;
        Py_ssize_t pos = 0;
        bool first = true;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            if (!first) buf_push(buf, ',');
            first = false;

            // Key must be string
            if (PyUnicode_Check(key)) {
                Py_ssize_t klen;
                const char* ks = PyUnicode_AsUTF8AndSize(key, &klen);
                if (!ks) return -1;
                write_escaped_string(buf, ks, klen);
            } else {
                // Convert key to string
                PyObject* key_str = PyObject_Str(key);
                if (!key_str) return -1;
                Py_ssize_t klen;
                const char* ks = PyUnicode_AsUTF8AndSize(key_str, &klen);
                if (!ks) { Py_DECREF(key_str); return -1; }
                write_escaped_string(buf, ks, klen);
                Py_DECREF(key_str);
            }

            buf_push(buf, ':');
            if (write_json(value, buf, depth + 1) < 0) return -1;
        }
        buf_push(buf, '}');
        return 0;
    }

    // List
    if (PyList_Check(obj)) {
        buf_push(buf, '[');
        Py_ssize_t len = PyList_GET_SIZE(obj);
        // Aggressive pre-sizing: dict items with string keys+values average ~120 bytes each
        if (len > 4) buf.reserve(buf.size() + len * 128);
        for (Py_ssize_t i = 0; i < len; i++) {
            if (i > 0) buf_push(buf, ',');
            PyObject* item = PyList_GET_ITEM(obj, i);  // borrowed ref
            if (write_json(item, buf, depth + 1) < 0) return -1;
        }
        buf_push(buf, ']');
        return 0;
    }

    // Tuple (treat like list)
    if (PyTuple_Check(obj)) {
        buf_push(buf, '[');
        Py_ssize_t len = PyTuple_GET_SIZE(obj);
        for (Py_ssize_t i = 0; i < len; i++) {
            if (i > 0) buf_push(buf, ',');
            PyObject* item = PyTuple_GET_ITEM(obj, i);
            if (write_json(item, buf, depth + 1) < 0) return -1;
        }
        buf_push(buf, ']');
        return 0;
    }

    // ── Special types (datetime, Decimal, UUID, Enum) ──────────────────────
    // Types are initialized at module load via json_writer_init()

    // Enum → serialize .value (must check before other types since Enum can wrap int/str)
    if (s_enum_type && PyObject_IsInstance(obj, s_enum_type)) {
        PyRef val(PyObject_GetAttr(obj, s_value));
        if (!val) return -1;
        return write_json(val.get(), buf, depth);
    }

    // datetime.datetime / datetime.date / datetime.time → "isoformat()"
    if ((s_datetime_type && PyObject_IsInstance(obj, s_datetime_type)) ||
        (s_date_type && PyObject_IsInstance(obj, s_date_type)) ||
        (s_time_type && PyObject_IsInstance(obj, s_time_type))) {
        PyRef iso(PyObject_CallMethodNoArgs(obj, s_isoformat));
        if (!iso) return -1;
        Py_ssize_t slen;
        const char* s = PyUnicode_AsUTF8AndSize(iso.get(), &slen);
        if (!s) return -1;
        write_escaped_string(buf, s, slen);
        return 0;
    }

    // decimal.Decimal → unquoted numeric string (JSON number)
    if (s_decimal_type && PyObject_IsInstance(obj, s_decimal_type)) {
        PyRef str_val(PyObject_Str(obj));
        if (!str_val) return -1;
        Py_ssize_t slen;
        const char* s = PyUnicode_AsUTF8AndSize(str_val.get(), &slen);
        if (!s) return -1;
        // Check for special Decimal values (NaN, Infinity, -Infinity)
        if (slen > 0 && (s[0] == 'N' || s[0] == 'I' || (s[0] == '-' && slen > 1 && s[1] == 'I'))) {
            PyErr_Format(PyExc_ValueError, "%s is not JSON serializable", s);
            return -1;
        }
        buf_append(buf, s, (size_t)slen);
        return 0;
    }

    // uuid.UUID → quoted string
    if (s_uuid_type && PyObject_IsInstance(obj, s_uuid_type)) {
        PyRef str_val(PyObject_Str(obj));
        if (!str_val) return -1;
        Py_ssize_t slen;
        const char* s = PyUnicode_AsUTF8AndSize(str_val.get(), &slen);
        if (!s) return -1;
        write_escaped_string(buf, s, slen);
        return 0;
    }

    // Fallback: try str()
    PyObject* str_repr = PyObject_Str(obj);
    if (!str_repr) return -1;
    Py_ssize_t slen;
    const char* s = PyUnicode_AsUTF8AndSize(str_repr, &slen);
    if (!s) { Py_DECREF(str_repr); return -1; }
    write_escaped_string(buf, s, slen);
    Py_DECREF(str_repr);
    return 0;
}

void json_writer_cleanup() {
    Py_CLEAR(s_datetime_type);
    Py_CLEAR(s_date_type);
    Py_CLEAR(s_time_type);
    Py_CLEAR(s_decimal_type);
    Py_CLEAR(s_uuid_type);
    Py_CLEAR(s_enum_type);
    Py_CLEAR(s_isoformat);
    Py_CLEAR(s_value);
}

PyObject* serialize_to_json_pybytes(PyObject* obj) {
    auto buf = acquire_buffer();
    if (write_json(obj, buf, 0) < 0) {
        release_buffer(std::move(buf));
        return nullptr;
    }
    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}
