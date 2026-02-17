#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

// ══════════════════════════════════════════════════════════════════════════════
// Concurrent route parameter registry
// ══════════════════════════════════════════════════════════════════════════════

struct ParamSpec {
    std::string field_name;
    std::string alias;
    std::string header_lookup_key;
    int location;   // 0=query, 1=header, 2=cookie, 3=path
    int type_tag;   // 0=str, 1=int, 2=float, 3=bool
    bool required;
    PyObject* default_value;  // strong ref or nullptr
};

struct RouteParamRegistry {
    std::vector<ParamSpec> specs;
};

static std::unordered_map<uint64_t, RouteParamRegistry> g_registry;
static std::shared_mutex g_registry_mutex;

// ══════════════════════════════════════════════════════════════════════════════
// Scalar coercion helper
// ══════════════════════════════════════════════════════════════════════════════

static PyObject* coerce_value(PyObject* val, int type_tag) {
    if (!PyUnicode_Check(val)) {
        Py_INCREF(val);
        return val;
    }

    const char* s;
    Py_ssize_t slen;
    s = PyUnicode_AsUTF8AndSize(val, &slen);
    if (!s) { Py_INCREF(val); return val; }

    switch (type_tag) {
        case 1: {  // int
            char* endptr;
            long long v = strtoll(s, &endptr, 10);
            if (endptr == s + slen) return PyLong_FromLongLong(v);
            Py_INCREF(val);
            return val;
        }
        case 2: {  // float
            char* endptr;
            double v = strtod(s, &endptr);
            if (endptr == s + slen) return PyFloat_FromDouble(v);
            Py_INCREF(val);
            return val;
        }
        case 3: {  // bool
            if ((slen == 4 && (memcmp(s, "true", 4) == 0 || memcmp(s, "True", 4) == 0)) ||
                (slen == 1 && s[0] == '1') ||
                (slen == 3 && (memcmp(s, "yes", 3) == 0 || memcmp(s, "Yes", 3) == 0))) {
                Py_RETURN_TRUE;
            }
            Py_RETURN_FALSE;
        }
        default:
            Py_INCREF(val);
            return val;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// register_route_params(route_id: u64, field_specs_list: PyList) → None
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_register_route_params(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"route_id", "field_specs_list", nullptr};
    unsigned long long route_id;
    PyObject* specs_list;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "KO", (char**)kwlist,
            &route_id, &specs_list)) return nullptr;

    if (!PyList_Check(specs_list)) {
        PyErr_SetString(PyExc_TypeError, "expected list");
        return nullptr;
    }

    RouteParamRegistry reg;
    Py_ssize_t n = PyList_GET_SIZE(specs_list);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* spec_dict = PyList_GET_ITEM(specs_list, i);
        if (!PyDict_Check(spec_dict)) continue;

        ParamSpec ps;
        PyObject* fn = PyDict_GetItemString(spec_dict, "field_name");
        PyObject* al = PyDict_GetItemString(spec_dict, "alias");
        PyObject* hlk = PyDict_GetItemString(spec_dict, "header_lookup_key");
        PyObject* loc = PyDict_GetItemString(spec_dict, "location");
        PyObject* tt = PyDict_GetItemString(spec_dict, "type_tag");
        PyObject* req = PyDict_GetItemString(spec_dict, "required");
        PyObject* def = PyDict_GetItemString(spec_dict, "default_value");

        ps.field_name = fn ? PyUnicode_AsUTF8(fn) : "";
        ps.alias = al && PyUnicode_Check(al) ? PyUnicode_AsUTF8(al) : ps.field_name;
        ps.header_lookup_key = hlk && PyUnicode_Check(hlk) ? PyUnicode_AsUTF8(hlk) : "";
        ps.location = loc ? (int)PyLong_AsLong(loc) : 0;
        ps.type_tag = tt ? (int)PyLong_AsLong(tt) : 0;
        ps.required = req ? PyObject_IsTrue(req) : false;
        if (def && def != Py_None) {
            Py_INCREF(def);
            ps.default_value = def;
        } else {
            ps.default_value = nullptr;
        }

        reg.specs.push_back(std::move(ps));
    }

    {
        std::unique_lock lock(g_registry_mutex);
        // Clean up old entry if exists
        auto it = g_registry.find(route_id);
        if (it != g_registry.end()) {
            for (auto& ps : it->second.specs) {
                Py_XDECREF(ps.default_value);
            }
        }
        g_registry[route_id] = std::move(reg);
    }

    Py_RETURN_NONE;
}

// ══════════════════════════════════════════════════════════════════════════════
// unregister_route_params(route_id: u64) → None
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_unregister_route_params(PyObject* self, PyObject* arg) {
    unsigned long long route_id = PyLong_AsUnsignedLongLong(arg);
    if (route_id == (unsigned long long)-1 && PyErr_Occurred()) return nullptr;

    std::unique_lock lock(g_registry_mutex);
    auto it = g_registry.find(route_id);
    if (it != g_registry.end()) {
        for (auto& ps : it->second.specs) {
            Py_XDECREF(ps.default_value);
        }
        g_registry.erase(it);
    }

    Py_RETURN_NONE;
}

// ══════════════════════════════════════════════════════════════════════════════
// Internal: extract params using specs from a source dict by location
// ══════════════════════════════════════════════════════════════════════════════

static void extract_from_source(PyObject* result, PyObject* source,
                                 const std::vector<ParamSpec>& specs, int location) {
    if (!source || source == Py_None || !PyDict_Check(source)) return;

    for (const auto& ps : specs) {
        if (ps.location != location) continue;

        const char* lookup_key = ps.alias.empty() ? ps.field_name.c_str() : ps.alias.c_str();
        if (location == 1) {  // header
            lookup_key = ps.header_lookup_key.empty() ? ps.field_name.c_str() : ps.header_lookup_key.c_str();
        }

        PyRef py_lookup(PyUnicode_FromString(lookup_key));
        PyObject* val = PyDict_GetItem(source, py_lookup.get());  // borrowed

        if (val) {
            // If val is a list, take first element
            if (PyList_Check(val) && PyList_GET_SIZE(val) > 0) {
                val = PyList_GET_ITEM(val, 0);
            }
            PyObject* coerced = coerce_value(val, ps.type_tag);
            if (coerced) {
                PyRef py_fname(PyUnicode_FromString(ps.field_name.c_str()));
                PyDict_SetItem(result, py_fname.get(), coerced);
                Py_DECREF(coerced);
            }
        } else if (ps.default_value) {
            PyRef py_fname(PyUnicode_FromString(ps.field_name.c_str()));
            PyDict_SetItem(result, py_fname.get(), ps.default_value);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// batch_extract_all_params(route_id, query_params, headers, cookies, path_params)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_batch_extract_all_params(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "route_id", "query_params", "headers", "cookies", "path_params", nullptr
    };

    unsigned long long route_id;
    PyObject* query_params;
    PyObject* headers;
    PyObject* cookies;
    PyObject* path_params;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "KOOOO", (char**)kwlist,
            &route_id, &query_params, &headers, &cookies, &path_params)) {
        return nullptr;
    }

    std::shared_lock lock(g_registry_mutex);
    auto it = g_registry.find(route_id);
    if (it == g_registry.end()) {
        lock.unlock();
        return PyDict_New();  // empty dict
    }

    const auto& specs = it->second.specs;

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    extract_from_source(result.get(), query_params, specs, 0);   // query
    extract_from_source(result.get(), headers, specs, 1);         // header
    extract_from_source(result.get(), cookies, specs, 2);         // cookie
    extract_from_source(result.get(), path_params, specs, 3);     // path

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// batch_extract_params_inline(query_params, headers, cookies, path_params, field_specs)
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_batch_extract_params_inline(PyObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {
        "query_params", "headers", "cookies", "path_params", "field_specs", nullptr
    };

    PyObject* query_params;
    PyObject* headers;
    PyObject* cookies;
    PyObject* path_params;
    PyObject* field_specs_list;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOOOO", (char**)kwlist,
            &query_params, &headers, &cookies, &path_params, &field_specs_list)) {
        return nullptr;
    }

    if (!PyList_Check(field_specs_list)) {
        PyErr_SetString(PyExc_TypeError, "field_specs must be a list");
        return nullptr;
    }

    // Build temporary specs
    std::vector<ParamSpec> specs;
    Py_ssize_t n = PyList_GET_SIZE(field_specs_list);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* sd = PyList_GET_ITEM(field_specs_list, i);
        if (!PyDict_Check(sd)) continue;

        ParamSpec ps;
        PyObject* fn = PyDict_GetItemString(sd, "field_name");
        PyObject* al = PyDict_GetItemString(sd, "alias");
        PyObject* hlk = PyDict_GetItemString(sd, "header_lookup_key");
        PyObject* loc = PyDict_GetItemString(sd, "location");
        PyObject* tt = PyDict_GetItemString(sd, "type_tag");
        PyObject* def = PyDict_GetItemString(sd, "default_value");

        ps.field_name = fn ? PyUnicode_AsUTF8(fn) : "";
        ps.alias = al && PyUnicode_Check(al) ? PyUnicode_AsUTF8(al) : ps.field_name;
        ps.header_lookup_key = hlk && PyUnicode_Check(hlk) ? PyUnicode_AsUTF8(hlk) : "";
        ps.location = loc ? (int)PyLong_AsLong(loc) : 0;
        ps.type_tag = tt ? (int)PyLong_AsLong(tt) : 0;
        ps.default_value = (def && def != Py_None) ? def : nullptr;

        specs.push_back(std::move(ps));
    }

    PyRef result(PyDict_New());
    if (!result) return nullptr;

    extract_from_source(result.get(), query_params, specs, 0);
    extract_from_source(result.get(), headers, specs, 1);
    extract_from_source(result.get(), cookies, specs, 2);
    extract_from_source(result.get(), path_params, specs, 3);

    return result.release();
}

// ══════════════════════════════════════════════════════════════════════════════
// Module shutdown: release all PyObject* refs held in the global registry
// ══════════════════════════════════════════════════════════════════════════════

void cleanup_param_registry() {
    std::unique_lock lock(g_registry_mutex);
    for (auto& [id, reg] : g_registry) {
        for (auto& ps : reg.specs) {
            Py_XDECREF(ps.default_value);
            ps.default_value = nullptr;
        }
    }
    g_registry.clear();
}
