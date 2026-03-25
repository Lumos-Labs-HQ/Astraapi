#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <cstring>
#include <string>

// ══════════════════════════════════════════════════════════════════════════════
// Base64 decode (for Basic auth)
// ══════════════════════════════════════════════════════════════════════════════

static const uint8_t b64_table[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64, 0,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
};

static std::string base64_decode(const char* input, size_t len) {
    std::string output;
    output.reserve(len * 3 / 4);

    uint32_t buf = 0;
    int bits = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = b64_table[(uint8_t)input[i]];
        if (c == 64) continue;  // skip invalid chars, padding
        buf = (buf << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    return output;
}

// ══════════════════════════════════════════════════════════════════════════════
// extract_bearer_token(auth_header: str) → Optional[str]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_extract_bearer_token(PyObject* self, PyObject* arg) {
    const char* header;
    Py_ssize_t header_len;
    if (PyUnicode_Check(arg)) {
        header = PyUnicode_AsUTF8AndSize(arg, &header_len);
        if (!header) return nullptr;
    } else {
        Py_RETURN_NONE;
    }

    // Check "Bearer " prefix (case-insensitive)
    if (header_len < 7) Py_RETURN_NONE;

    // Compare first 7 chars case-insensitively
    const char* prefix = "bearer ";
    for (int i = 0; i < 7; i++) {
        char c = header[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != prefix[i]) Py_RETURN_NONE;
    }

    // Return the token after "Bearer "
    return PyUnicode_FromStringAndSize(header + 7, header_len - 7);
}

// ══════════════════════════════════════════════════════════════════════════════
// extract_basic_credentials(auth_header: str) → Optional[Tuple[str, str]]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_extract_basic_credentials(PyObject* self, PyObject* arg) {
    const char* header;
    Py_ssize_t header_len;
    if (PyUnicode_Check(arg)) {
        header = PyUnicode_AsUTF8AndSize(arg, &header_len);
        if (!header) return nullptr;
    } else {
        Py_RETURN_NONE;
    }

    // Check "Basic " prefix
    if (header_len < 6) Py_RETURN_NONE;
    const char* prefix = "basic ";
    for (int i = 0; i < 6; i++) {
        char c = header[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != prefix[i]) Py_RETURN_NONE;
    }

    // Base64 decode the credentials
    std::string decoded = base64_decode(header + 6, header_len - 6);

    // Split at first ':'
    size_t colon = decoded.find(':');
    if (colon == std::string::npos) Py_RETURN_NONE;

    PyRef username(PyUnicode_FromStringAndSize(decoded.c_str(), colon));
    PyRef password(PyUnicode_FromStringAndSize(decoded.c_str() + colon + 1, decoded.size() - colon - 1));
    if (!username || !password) return nullptr;

    return PyTuple_Pack(2, username.get(), password.get());
}

// ══════════════════════════════════════════════════════════════════════════════
// get_authorization_scheme_param(authorization: str) → Tuple[str, str]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_get_authorization_scheme_param(PyObject* self, PyObject* arg) {
    const char* auth;
    Py_ssize_t auth_len;
    if (PyUnicode_Check(arg)) {
        auth = PyUnicode_AsUTF8AndSize(arg, &auth_len);
        if (!auth) return nullptr;
    } else {
        PyRef empty(PyUnicode_FromString(""));
        return PyTuple_Pack(2, empty.get(), empty.get());
    }

    // Find first space
    const char* space = (const char*)memchr(auth, ' ', auth_len);
    if (!space) {
        PyRef scheme(PyUnicode_FromStringAndSize(auth, auth_len));
        PyRef param(PyUnicode_FromString(""));
        return PyTuple_Pack(2, scheme.get(), param.get());
    }

    PyRef scheme(PyUnicode_FromStringAndSize(auth, space - auth));
    // Skip whitespace after scheme
    const char* param_start = space + 1;
    while (param_start < auth + auth_len && *param_start == ' ') param_start++;

    PyRef param(PyUnicode_FromStringAndSize(param_start, auth + auth_len - param_start));
    if (!scheme || !param) return nullptr;

    return PyTuple_Pack(2, scheme.get(), param.get());
}

// ══════════════════════════════════════════════════════════════════════════════
// extract_all_security_credentials(headers: PyDict) → PyDict
// ══════════════════════════════════════════════════════════════════════════════

