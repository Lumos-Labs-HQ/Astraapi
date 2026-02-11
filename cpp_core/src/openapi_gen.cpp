#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "json_parser.hpp"
#include "json_writer.hpp"
#include "buffer_pool.hpp"
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Helper: write JSON, skipping None values in dicts (OpenAPI-clean output)
// ══════════════════════════════════════════════════════════════════════════════

static int write_openapi_json(PyObject* obj, std::vector<char>& buf, int depth);

static void write_json_str_oa(std::vector<char>& buf, const char* s, Py_ssize_t len) {
    buf.push_back('"');
    for (Py_ssize_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { buf.push_back('\\'); buf.push_back('"'); }
        else if (c == '\\') { buf.push_back('\\'); buf.push_back('\\'); }
        else if (c == '\n') { buf.push_back('\\'); buf.push_back('n'); }
        else if (c == '\r') { buf.push_back('\\'); buf.push_back('r'); }
        else if (c == '\t') { buf.push_back('\\'); buf.push_back('t'); }
        else if (c < 0x20) {
            char hex[7];
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            buf.insert(buf.end(), hex, hex + 6);
        }
        else buf.push_back(c);
    }
    buf.push_back('"');
}

static int write_openapi_json(PyObject* obj, std::vector<char>& buf, int depth) {
    if (depth > 64) return -1;

    if (obj == Py_None) {
        // Skip None in OpenAPI context — caller handles this
        const char* null_str = "null";
        buf.insert(buf.end(), null_str, null_str + 4);
        return 0;
    }

    if (PyUnicode_Check(obj)) {
        const char* s;
        Py_ssize_t len;
        s = PyUnicode_AsUTF8AndSize(obj, &len);
        if (!s) return -1;
        write_json_str_oa(buf, s, len);
        return 0;
    }

    if (PyDict_Check(obj)) {
        buf.push_back('{');
        PyObject* key;
        PyObject* value;
        Py_ssize_t pos = 0;
        bool first = true;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            // Skip None values for clean OpenAPI output
            if (value == Py_None) continue;
            if (!first) buf.push_back(',');
            first = false;

            // Key
            if (PyUnicode_Check(key)) {
                const char* ks;
                Py_ssize_t klen;
                ks = PyUnicode_AsUTF8AndSize(key, &klen);
                if (!ks) return -1;
                write_json_str_oa(buf, ks, klen);
            } else {
                PyRef ks(PyObject_Str(key));
                if (!ks) return -1;
                const char* s;
                Py_ssize_t len;
                s = PyUnicode_AsUTF8AndSize(ks.get(), &len);
                write_json_str_oa(buf, s, len);
            }
            buf.push_back(':');
            if (write_openapi_json(value, buf, depth + 1) < 0) return -1;
        }
        buf.push_back('}');
        return 0;
    }

    // For everything else, delegate to standard write_json
    return write_json(obj, buf, depth);
}

// ══════════════════════════════════════════════════════════════════════════════
// build_openapi_schema(...) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_build_openapi_schema(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "title", "version", "openapi_version",
        "summary", "description", "terms_of_service",
        "contact", "license_info", "routes_info", "schemas",
        "servers", "tags", "external_docs", nullptr
    };

    const char* title;
    const char* version;
    const char* openapi_version = "3.1.0";
    const char* summary = nullptr;
    const char* description = nullptr;
    const char* terms_of_service = nullptr;
    PyObject* contact = Py_None;
    PyObject* license_info = Py_None;
    PyObject* routes_info = Py_None;
    PyObject* schemas = Py_None;
    PyObject* servers = Py_None;
    PyObject* tags = Py_None;
    PyObject* external_docs = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "ss|szzzOOOOOOO", (char**)kwlist,
            &title, &version, &openapi_version,
            &summary, &description, &terms_of_service,
            &contact, &license_info, &routes_info, &schemas,
            &servers, &tags, &external_docs)) {
        return nullptr;
    }

    // Build the OpenAPI schema dict in Python, then serialize
    PyRef schema(PyDict_New());
    if (!schema) return nullptr;

    // openapi version
    PyRef oa_ver(PyUnicode_FromString(openapi_version));
    PyDict_SetItemString(schema.get(), "openapi", oa_ver.get());

    // info object
    PyRef info(PyDict_New());
    PyRef py_title(PyUnicode_FromString(title));
    PyRef py_version(PyUnicode_FromString(version));
    PyDict_SetItemString(info.get(), "title", py_title.get());
    PyDict_SetItemString(info.get(), "version", py_version.get());

    if (summary) {
        PyRef s(PyUnicode_FromString(summary));
        PyDict_SetItemString(info.get(), "summary", s.get());
    }
    if (description) {
        PyRef d(PyUnicode_FromString(description));
        PyDict_SetItemString(info.get(), "description", d.get());
    }
    if (terms_of_service) {
        PyRef t(PyUnicode_FromString(terms_of_service));
        PyDict_SetItemString(info.get(), "termsOfService", t.get());
    }
    if (contact != Py_None && PyDict_Check(contact)) {
        PyDict_SetItemString(info.get(), "contact", contact);
    }
    if (license_info != Py_None && PyDict_Check(license_info)) {
        PyDict_SetItemString(info.get(), "license", license_info);
    }
    PyDict_SetItemString(schema.get(), "info", info.get());

    // paths (from routes_info)
    if (routes_info != Py_None && PyList_Check(routes_info)) {
        PyRef paths(PyDict_New());
        Py_ssize_t n = PyList_GET_SIZE(routes_info);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* route = PyList_GET_ITEM(routes_info, i);
            if (!PyDict_Check(route)) continue;

            PyObject* path_obj = PyDict_GetItemString(route, "path");
            PyObject* methods_obj = PyDict_GetItemString(route, "methods");
            if (!path_obj || !methods_obj) continue;

            PyObject* path_entry = PyDict_GetItem(paths.get(), path_obj);
            if (!path_entry) {
                path_entry = PyDict_New();
                PyDict_SetItem(paths.get(), path_obj, path_entry);
                Py_DECREF(path_entry);
            }

            // Add method entries
            if (PyList_Check(methods_obj)) {
                Py_ssize_t nm = PyList_GET_SIZE(methods_obj);
                for (Py_ssize_t j = 0; j < nm; j++) {
                    PyObject* method = PyList_GET_ITEM(methods_obj, j);
                    const char* ms = PyUnicode_AsUTF8(method);
                    if (!ms) continue;
                    std::string m_lower(ms);
                    for (auto& c : m_lower) if (c >= 'A' && c <= 'Z') c += 32;

                    // Build operation object from route metadata
                    PyRef op(PyDict_New());

                    PyObject* op_summary = PyDict_GetItemString(route, "summary");
                    PyObject* op_desc = PyDict_GetItemString(route, "description");
                    PyObject* op_id = PyDict_GetItemString(route, "operation_id");
                    PyObject* op_tags = PyDict_GetItemString(route, "tags");

                    if (op_summary && op_summary != Py_None)
                        PyDict_SetItemString(op.get(), "summary", op_summary);
                    if (op_desc && op_desc != Py_None)
                        PyDict_SetItemString(op.get(), "description", op_desc);
                    if (op_id && op_id != Py_None)
                        PyDict_SetItemString(op.get(), "operationId", op_id);
                    if (op_tags && op_tags != Py_None)
                        PyDict_SetItemString(op.get(), "tags", op_tags);

                    // responses placeholder
                    PyRef responses(PyDict_New());
                    PyRef default_resp(PyDict_New());
                    PyRef desc_str(PyUnicode_FromString("Successful Response"));
                    PyDict_SetItemString(default_resp.get(), "description", desc_str.get());
                    PyDict_SetItemString(responses.get(), "200", default_resp.get());
                    PyDict_SetItemString(op.get(), "responses", responses.get());

                    PyRef m_key(PyUnicode_FromString(m_lower.c_str()));
                    PyDict_SetItem(path_entry, m_key.get(), op.get());
                }
            }
        }
        PyDict_SetItemString(schema.get(), "paths", paths.get());
    } else {
        PyRef empty_paths(PyDict_New());
        PyDict_SetItemString(schema.get(), "paths", empty_paths.get());
    }

    // components/schemas
    if (schemas != Py_None && PyDict_Check(schemas)) {
        PyRef components(PyDict_New());
        PyDict_SetItemString(components.get(), "schemas", schemas);
        PyDict_SetItemString(schema.get(), "components", components.get());
    }

    // servers
    if (servers != Py_None && PyList_Check(servers)) {
        PyDict_SetItemString(schema.get(), "servers", servers);
    }

    // tags
    if (tags != Py_None && PyList_Check(tags)) {
        PyDict_SetItemString(schema.get(), "tags", tags);
    }

    // externalDocs
    if (external_docs != Py_None && PyDict_Check(external_docs)) {
        PyDict_SetItemString(schema.get(), "externalDocs", external_docs);
    }

    // Serialize to JSON bytes (None-excluding)
    auto buf = acquire_buffer();
    if (write_openapi_json(schema.get(), buf, 0) < 0) {
        release_buffer(std::move(buf));
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Failed to serialize OpenAPI schema");
        return nullptr;
    }

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// build_openapi_schema_pretty — same but indented
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_build_openapi_schema_pretty(PyObject* self, PyObject* args, PyObject* kwargs) {
    // Build schema dict using same logic
    PyRef schema_bytes(py_build_openapi_schema(self, args, kwargs));
    if (!schema_bytes) return nullptr;

    // Parse bytes back to dict (yyjson), then pretty-print with json.dumps
    static PyObject* json_dumps = nullptr;
    if (!json_dumps) {
        PyRef json_mod(PyImport_ImportModule("json"));
        if (!json_mod) return nullptr;
        json_dumps = PyObject_GetAttrString(json_mod.get(), "dumps");
    }

    // Extract raw bytes from schema_bytes (PyBytes) and parse with yyjson
    char* schema_data = nullptr;
    Py_ssize_t schema_len = 0;
    if (PyBytes_AsStringAndSize(schema_bytes.get(), &schema_data, &schema_len) < 0) return nullptr;
    PyRef parsed(yyjson_parse_to_pyobject(schema_data, static_cast<size_t>(schema_len)));
    if (!parsed) return nullptr;

    PyRef dump_args(PyTuple_Pack(1, parsed.get()));
    PyRef dump_kwargs(PyDict_New());
    PyRef indent(PyLong_FromLong(2));
    PyDict_SetItemString(dump_kwargs.get(), "indent", indent.get());
    PyDict_SetItemString(dump_kwargs.get(), "ensure_ascii", Py_False);

    PyRef pretty(PyObject_Call(json_dumps, dump_args.get(), dump_kwargs.get()));
    if (!pretty) return nullptr;

    if (PyUnicode_Check(pretty.get())) {
        return PyUnicode_AsEncodedString(pretty.get(), "utf-8", "strict");
    }

    Py_INCREF(pretty.get());
    return pretty.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// openapi_dict_to_json_bytes(openapi_dict: PyDict) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_openapi_dict_to_json_bytes(PyObject* self, PyObject* arg) {
    if (!PyDict_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected dict");
        return nullptr;
    }

    auto buf = acquire_buffer();
    if (write_openapi_json(arg, buf, 0) < 0) {
        release_buffer(std::move(buf));
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Failed to serialize OpenAPI dict");
        return nullptr;
    }

    PyObject* result = PyBytes_FromStringAndSize(buf.data(), (Py_ssize_t)buf.size());
    release_buffer(std::move(buf));
    return result;
}
