#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "percent_decode.hpp"
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>

// Portable memmem
static const void* safe_memmem(const void* haystack, size_t haystacklen,
                               const void* needle, size_t needlelen) {
    if (needlelen == 0) return haystack;
    if (haystacklen < needlelen) return nullptr;
    const char* h = (const char*)haystack;
    const char* n = (const char*)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (memcmp(h + i, n, needlelen) == 0) return (const void*)(h + i);
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_urlencoded_body(body: bytes) → PyList of (str, str) tuples
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_urlencoded_body(PyObject* self, PyObject* arg) {
    char* data;
    Py_ssize_t data_len;

    if (PyBytes_Check(arg)) {
        PyBytes_AsStringAndSize(arg, &data, &data_len);
    } else if (PyUnicode_Check(arg)) {
        data = (char*)PyUnicode_AsUTF8AndSize(arg, &data_len);
        if (!data) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected bytes or str");
        return nullptr;
    }

    // Parse into list of (key, value) tuples
    PyRef result(PyList_New(0));
    if (!result) return nullptr;

    const char* p = data;
    const char* end = data + data_len;

    while (p < end) {
        const char* key_start = p;
        const char* eq = nullptr;
        while (p < end && *p != '&') {
            if (*p == '=' && !eq) eq = p;
            p++;
        }

        std::string key, val;
        if (eq) {
            key = percent_decode(key_start, eq - key_start);
            val = percent_decode(eq + 1, p - eq - 1);
        } else {
            key = percent_decode(key_start, p - key_start);
        }

        PyRef pk(PyUnicode_FromStringAndSize(key.c_str(), key.size()));
        PyRef pv(PyUnicode_FromStringAndSize(val.c_str(), val.size()));
        if (!pk || !pv) return nullptr;

        PyRef tup(PyTuple_Pack(2, pk.get(), pv.get()));
        if (!tup) return nullptr;
        PyList_Append(result.get(), tup.get());

        if (p < end) p++;
    }

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// parse_multipart_body(body: bytes, boundary: str) → PyList of dicts
// Each dict: {name, data, filename, content_type, headers}
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_parse_multipart_body(PyObject* self, PyObject* args) {
    PyObject* body_obj;
    const char* boundary;
    Py_ssize_t boundary_len;

    if (!PyArg_ParseTuple(args, "Os#", &body_obj, &boundary, &boundary_len)) return nullptr;

    char* data;
    Py_ssize_t data_len;
    if (PyBytes_Check(body_obj)) {
        PyBytes_AsStringAndSize(body_obj, &data, &data_len);
    } else {
        PyErr_SetString(PyExc_TypeError, "expected bytes body");
        return nullptr;
    }

    PyRef result(PyList_New(0));
    if (!result) return nullptr;

    // Build delimiter: "--" + boundary
    std::string delim = "--";
    delim.append(boundary, boundary_len);

    // Find parts between delimiters
    const char* p = data;
    const char* end = data + data_len;

    // Skip preamble — find first delimiter
    const char* first = (const char*)safe_memmem(p, end - p, delim.c_str(), delim.size());
    if (!first) return result.release();  // no parts

    p = first + delim.size();

    while (p < end) {
        // Check for terminal "--"
        if (p + 2 <= end && p[0] == '-' && p[1] == '-') break;

        // Skip CRLF after delimiter
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        // Parse headers until empty line
        std::string name, filename, content_type;
        PyRef headers_dict(PyDict_New());

        while (p < end) {
            const char* line_start = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            size_t line_len = p - line_start;

            // Skip CRLF
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;

            if (line_len == 0) break;  // empty line — end of headers

            // Parse header line
            std::string line(line_start, line_len);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string hname = line.substr(0, colon);
                std::string hval = line.substr(colon + 1);
                // Trim leading space
                while (!hval.empty() && hval[0] == ' ') hval.erase(0, 1);

                // Lowercase header name for comparison
                std::string hname_lower = hname;
                for (auto& c : hname_lower) if (c >= 'A' && c <= 'Z') c += 32;

                PyRef pk(PyUnicode_FromString(hname_lower.c_str()));
                PyRef pv(PyUnicode_FromString(hval.c_str()));
                if (pk && pv) PyDict_SetItem(headers_dict.get(), pk.get(), pv.get());

                if (hname_lower == "content-disposition") {
                    // Parse name="..." and filename="..."
                    size_t npos = hval.find("name=\"");
                    if (npos != std::string::npos) {
                        npos += 6;
                        size_t nend = hval.find('"', npos);
                        if (nend != std::string::npos) name = hval.substr(npos, nend - npos);
                    }
                    size_t fpos = hval.find("filename=\"");
                    if (fpos != std::string::npos) {
                        fpos += 10;
                        size_t fend = hval.find('"', fpos);
                        if (fend != std::string::npos) filename = hval.substr(fpos, fend - fpos);
                    }
                } else if (hname_lower == "content-type") {
                    content_type = hval;
                }
            }
        }

        // Find next delimiter — body is everything until then
        const char* next_delim = (const char*)safe_memmem(p, end - p, delim.c_str(), delim.size());
        if (!next_delim) next_delim = end;

        // Body: strip trailing CRLF before delimiter
        const char* body_end = next_delim;
        if (body_end > p && *(body_end - 1) == '\n') body_end--;
        if (body_end > p && *(body_end - 1) == '\r') body_end--;

        // Build part dict
        PyRef part(PyDict_New());
        if (!part) return nullptr;

        PyRef py_name(PyUnicode_FromString(name.c_str()));
        PyRef py_data(PyBytes_FromStringAndSize(p, body_end - p));
        PyDict_SetItemString(part.get(), "name", py_name.get());
        PyDict_SetItemString(part.get(), "data", py_data.get());

        if (!filename.empty()) {
            PyRef py_fn(PyUnicode_FromString(filename.c_str()));
            PyDict_SetItemString(part.get(), "filename", py_fn.get());
        } else {
            PyDict_SetItemString(part.get(), "filename", Py_None);
        }

        if (!content_type.empty()) {
            PyRef py_ct(PyUnicode_FromString(content_type.c_str()));
            PyDict_SetItemString(part.get(), "content_type", py_ct.get());
        } else {
            PyDict_SetItemString(part.get(), "content_type", Py_None);
        }

        PyDict_SetItemString(part.get(), "headers", headers_dict.get());
        PyList_Append(result.get(), part.get());

        // Advance past delimiter
        p = next_delim + delim.size();
    }

    return result.release();
}
