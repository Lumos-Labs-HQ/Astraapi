#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "asgi_constants.hpp"
#include "app.hpp"
#include "json_writer.hpp"
#include "pyref.hpp"
#include <cstring>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Forward declarations for module-level functions (implemented in other files)
// ══════════════════════════════════════════════════════════════════════════════

// request_parser.cpp
extern PyObject* py_parse_request_full(PyObject* self, PyObject* args, PyObject* kwargs);

// middleware_engine.cpp
extern PyObject* py_gzip_compress(PyObject* self, PyObject* args);
extern PyObject* py_gzip_decompress(PyObject* self, PyObject* args);
extern PyObject* py_brotli_compress(PyObject* self, PyObject* args);
extern PyObject* py_negotiate_encoding(PyObject* self, PyObject* arg);
extern PyObject* py_should_compress(PyObject* self, PyObject* args);
extern PyObject* py_compress_response(PyObject* self, PyObject* args);

// security.cpp
extern PyObject* py_extract_bearer_token(PyObject* self, PyObject* arg);
extern PyObject* py_extract_basic_credentials(PyObject* self, PyObject* arg);
extern PyObject* py_get_authorization_scheme_param(PyObject* self, PyObject* arg);
extern PyObject* py_extract_all_security_credentials(PyObject* self, PyObject* arg);

// websocket_handler.cpp
extern PyObject* py_ws_parse_json(PyObject* self, PyObject* arg);
extern PyObject* py_ws_serialize_json(PyObject* self, PyObject* arg);
extern PyObject* py_ws_batch_parse(PyObject* self, PyObject* arg);
extern PyObject* py_ws_build_json_frame(PyObject* self, PyObject* args);

// ws_frame_parser.cpp
extern void ws_unmask(uint8_t* payload, size_t len, const uint8_t mask[4]);
extern PyObject* py_ws_echo_frames(PyObject* self, PyObject* arg);
extern PyObject* py_ws_build_frame_bytes(PyObject* self, PyObject* args);
extern PyObject* py_ws_build_ping_frame(PyObject* self, PyObject* arg);
extern PyObject* py_ws_build_close_frame_bytes(PyObject* self, PyObject* arg);
extern PyObject* py_ws_build_frames_batch(PyObject* self, PyObject* arg);

// ws_ring_buffer.cpp
extern PyObject* py_ws_ring_buffer_create(PyObject* self, PyObject* args);
extern PyObject* py_ws_ring_buffer_append(PyObject* self, PyObject* args);
extern PyObject* py_ws_ring_buffer_readable_region(PyObject* self, PyObject* args);
extern PyObject* py_ws_ring_buffer_consume(PyObject* self, PyObject* args);
extern PyObject* py_ws_ring_buffer_readable(PyObject* self, PyObject* args);
extern PyObject* py_ws_ring_buffer_reset(PyObject* self, PyObject* args);
extern PyObject* py_ws_echo_direct(PyObject* self, PyObject* args);
extern PyObject* py_ws_echo_direct_fd(PyObject* self, PyObject* args);
extern PyObject* py_ws_handle_direct(PyObject* self, PyObject* args);
extern PyObject* py_ws_handle_json_direct(PyObject* self, PyObject* args);

// dep_engine.cpp
extern PyObject* py_compile_dep_plan(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_unregister_dep_plan(PyObject* self, PyObject* arg);
extern PyObject* py_get_dep_plan(PyObject* self, PyObject* arg);
extern PyObject* py_prepare_dep_execution(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_dep_plan_count(PyObject* self, PyObject* args);

// openapi_gen.cpp
extern PyObject* py_build_openapi_schema(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_build_openapi_schema_pretty(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_openapi_dict_to_json_bytes(PyObject* self, PyObject* arg);

// params.cpp (v0.1 compat — implemented in utils.cpp)
extern PyObject* py_parse_query_string(PyObject* self, PyObject* args);
extern PyObject* py_parse_headers_to_dict(PyObject* self, PyObject* args);
extern PyObject* py_parse_scope_headers(PyObject* self, PyObject* args);
extern PyObject* py_batch_extract_params(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_parse_cookie_header(PyObject* self, PyObject* arg);

// json_encoder.cpp
extern PyObject* py_fast_jsonable_encode(PyObject* self, PyObject* arg);

// body_parser.cpp
extern PyObject* py_parse_json_body(PyObject* self, PyObject* arg);
extern PyObject* py_serialize_to_json_bytes(PyObject* self, PyObject* arg);
extern PyObject* py_serialize_to_json_bytes_pretty(PyObject* self, PyObject* arg);

// content_type (utils.cpp)
extern PyObject* py_parse_content_type(PyObject* self, PyObject* arg);
extern PyObject* py_is_json_content_type(PyObject* self, PyObject* arg);

// dependency_resolver.cpp
extern PyObject* py_compute_dependency_order(PyObject* self, PyObject* arg);

// response_pipeline.cpp
extern PyObject* py_encode_to_json_bytes(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_encode_to_json_bytes_pretty(PyObject* self, PyObject* args, PyObject* kwargs);

// request_pipeline.cpp
extern PyObject* py_process_request(PyObject* self, PyObject* args, PyObject* kwargs);

// form_parser.cpp
extern PyObject* py_parse_multipart_body(PyObject* self, PyObject* args);
extern PyObject* py_parse_urlencoded_body(PyObject* self, PyObject* arg);

// scalar_coerce (utils.cpp)
extern PyObject* py_batch_coerce_scalars(PyObject* self, PyObject* args);

// error_response.cpp
extern PyObject* py_serialize_error_response(PyObject* self, PyObject* arg);
extern PyObject* py_serialize_error_list(PyObject* self, PyObject* arg);

// param_extractor.cpp
extern PyObject* py_register_route_params(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_batch_extract_all_params(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_batch_extract_params_inline(PyObject* self, PyObject* args, PyObject* kwargs);
extern PyObject* py_unregister_route_params(PyObject* self, PyObject* arg);

// ══════════════════════════════════════════════════════════════════════════════
// CoreRouter — standalone trie router (v0.1 compat)
// ══════════════════════════════════════════════════════════════════════════════

typedef struct {
    PyObject_HEAD
    Router router;
    std::vector<int> route_indices;
} CoreRouterObject;

static PyObject* CoreRouter_new(PyTypeObject* type, PyObject*, PyObject*) {
    CoreRouterObject* self = (CoreRouterObject*)type->tp_alloc(type, 0);
    if (self) {
        new (&self->router) Router();
        new (&self->route_indices) std::vector<int>();
    }
    return (PyObject*)self;
}

static void CoreRouter_dealloc(CoreRouterObject* self) {
    self->router.~Router();
    self->route_indices.~vector();
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* CoreRouter_add_route(CoreRouterObject* self, PyObject* args) {
    const char* path;
    int index;
    if (!PyArg_ParseTuple(args, "si", &path, &index)) return nullptr;
    self->router.insert(path, index);
    self->route_indices.push_back(index);
    Py_RETURN_NONE;
}

static PyObject* CoreRouter_match_route(CoreRouterObject* self, PyObject* arg) {
    const char* path;
    if (PyUnicode_Check(arg)) {
        path = PyUnicode_AsUTF8(arg);
        if (!path) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return nullptr;
    }

    auto match = self->router.at(path, strlen(path));
    if (!match) Py_RETURN_NONE;

    // Return (route_index, path_params_dict)
    PyRef params_dict(PyDict_New());
    if (!params_dict) return nullptr;
    for (int pi = 0; pi < match->param_count; pi++) {
        auto k = match->params[pi].name;
        auto v = match->params[pi].value;
        PyRef pk(PyUnicode_FromStringAndSize(k.data(), k.size()));
        PyRef pv(PyUnicode_FromStringAndSize(v.data(), v.size()));
        if (!pk || !pv) return nullptr;
        if (PyDict_SetItem(params_dict.get(), pk.get(), pv.get()) < 0) return nullptr;
    }

    return Py_BuildValue("(iN)", match->route_index, params_dict.release());
}

static PyMethodDef CoreRouter_methods[] = {
    {"add_route", (PyCFunction)CoreRouter_add_route, METH_VARARGS, nullptr},
    {"match_route", (PyCFunction)CoreRouter_match_route, METH_O, nullptr},
    {nullptr}
};

static PyTypeObject CoreRouterType = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name = "_fastapi_core.CoreRouter",
    .tp_basicsize = sizeof(CoreRouterObject),
    .tp_dealloc = (destructor)CoreRouter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = CoreRouter_methods,
    .tp_new = CoreRouter_new,
};

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket unmask — Python-callable wrapper for ws_frame_parser.cpp
// ══════════════════════════════════════════════════════════════════════════════

static PyObject* py_ws_unmask(PyObject* /*self*/, PyObject* args) {
    // ws_unmask(payload: bytearray, mask: bytes) -> None
    // Unmasks payload in-place using 8-byte-at-a-time XOR
    Py_buffer payload_buf;
    const char* mask_data;
    Py_ssize_t mask_len;

    if (!PyArg_ParseTuple(args, "y*y#", &payload_buf, &mask_data, &mask_len)) {
        return nullptr;
    }

    if (mask_len != 4) {
        PyBuffer_Release(&payload_buf);
        PyErr_SetString(PyExc_ValueError, "mask must be exactly 4 bytes");
        return nullptr;
    }

    if (!payload_buf.readonly && payload_buf.buf && payload_buf.len > 0) {
        ws_unmask((uint8_t*)payload_buf.buf, (size_t)payload_buf.len,
                  (const uint8_t*)mask_data);
    }

    PyBuffer_Release(&payload_buf);
    Py_RETURN_NONE;
}

// ══════════════════════════════════════════════════════════════════════════════
// Module method table
// ══════════════════════════════════════════════════════════════════════════════

static PyMethodDef module_methods[] = {
    // v2.0: Request parser
    {"parse_request_full", (PyCFunction)py_parse_request_full, METH_VARARGS | METH_KEYWORDS, nullptr},

    // v2.0: Middleware engine
    {"gzip_compress", (PyCFunction)py_gzip_compress, METH_VARARGS, nullptr},
    {"gzip_decompress", (PyCFunction)py_gzip_decompress, METH_VARARGS, nullptr},
    {"brotli_compress", (PyCFunction)py_brotli_compress, METH_VARARGS, nullptr},
    {"negotiate_encoding", (PyCFunction)py_negotiate_encoding, METH_O, nullptr},
    {"should_compress", (PyCFunction)py_should_compress, METH_VARARGS, nullptr},
    {"compress_response", (PyCFunction)py_compress_response, METH_VARARGS, nullptr},

    // v2.0: Security
    {"extract_bearer_token", (PyCFunction)py_extract_bearer_token, METH_O, nullptr},
    {"extract_basic_credentials", (PyCFunction)py_extract_basic_credentials, METH_O, nullptr},
    {"get_authorization_scheme_param", (PyCFunction)py_get_authorization_scheme_param, METH_O, nullptr},
    {"extract_all_security_credentials", (PyCFunction)py_extract_all_security_credentials, METH_O, nullptr},

    // v2.0: WebSocket
    {"ws_parse_json", (PyCFunction)py_ws_parse_json, METH_O, nullptr},
    {"ws_serialize_json", (PyCFunction)py_ws_serialize_json, METH_O, nullptr},
    {"ws_batch_parse", (PyCFunction)py_ws_batch_parse, METH_O, nullptr},
    {"ws_unmask", (PyCFunction)py_ws_unmask, METH_VARARGS, nullptr},
    {"ws_echo_frames", (PyCFunction)py_ws_echo_frames, METH_O, nullptr},
    {"ws_build_frame_bytes", (PyCFunction)py_ws_build_frame_bytes, METH_VARARGS, nullptr},
    {"ws_build_ping_frame", (PyCFunction)py_ws_build_ping_frame, METH_O, nullptr},
    {"ws_build_close_frame_bytes", (PyCFunction)py_ws_build_close_frame_bytes, METH_O, nullptr},
    {"ws_build_frames_batch", (PyCFunction)py_ws_build_frames_batch, METH_O, nullptr},
    {"ws_build_json_frame", (PyCFunction)py_ws_build_json_frame, METH_VARARGS, nullptr},

    // v2.0: WebSocket Ring Buffer
    {"ws_ring_buffer_create", (PyCFunction)py_ws_ring_buffer_create, METH_NOARGS, nullptr},
    {"ws_ring_buffer_append", (PyCFunction)py_ws_ring_buffer_append, METH_VARARGS, nullptr},
    {"ws_ring_buffer_readable_region", (PyCFunction)py_ws_ring_buffer_readable_region, METH_VARARGS, nullptr},
    {"ws_ring_buffer_consume", (PyCFunction)py_ws_ring_buffer_consume, METH_VARARGS, nullptr},
    {"ws_ring_buffer_readable", (PyCFunction)py_ws_ring_buffer_readable, METH_VARARGS, nullptr},
    {"ws_ring_buffer_reset", (PyCFunction)py_ws_ring_buffer_reset, METH_VARARGS, nullptr},
    {"ws_echo_direct", (PyCFunction)py_ws_echo_direct, METH_VARARGS, nullptr},
    {"ws_echo_direct_fd", (PyCFunction)py_ws_echo_direct_fd, METH_VARARGS, nullptr},
    {"ws_handle_direct", (PyCFunction)py_ws_handle_direct, METH_VARARGS, nullptr},
    {"ws_handle_echo_direct", (PyCFunction)py_ws_echo_direct, METH_VARARGS, nullptr},
    {"ws_handle_json_direct", (PyCFunction)py_ws_handle_json_direct, METH_VARARGS, nullptr},

    // v2.0: Dependency engine
    {"compile_dep_plan", (PyCFunction)py_compile_dep_plan, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"unregister_dep_plan", (PyCFunction)py_unregister_dep_plan, METH_O, nullptr},
    {"get_dep_plan", (PyCFunction)py_get_dep_plan, METH_O, nullptr},
    {"prepare_dep_execution", (PyCFunction)py_prepare_dep_execution, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"dep_plan_count", (PyCFunction)py_dep_plan_count, METH_NOARGS, nullptr},

    // v2.0: OpenAPI
    {"build_openapi_schema", (PyCFunction)py_build_openapi_schema, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"build_openapi_schema_pretty", (PyCFunction)py_build_openapi_schema_pretty, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"openapi_dict_to_json_bytes", (PyCFunction)py_openapi_dict_to_json_bytes, METH_O, nullptr},

    // v0.1–v1.0: Params
    {"parse_query_string", (PyCFunction)py_parse_query_string, METH_VARARGS, nullptr},
    {"parse_headers_to_dict", (PyCFunction)py_parse_headers_to_dict, METH_VARARGS, nullptr},
    {"parse_scope_headers", (PyCFunction)py_parse_scope_headers, METH_VARARGS, nullptr},
    {"batch_extract_params", (PyCFunction)py_batch_extract_params, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"parse_cookie_header", (PyCFunction)py_parse_cookie_header, METH_O, nullptr},

    // v0.1–v1.0: JSON encoding
    {"fast_jsonable_encode", (PyCFunction)py_fast_jsonable_encode, METH_O, nullptr},

    // v0.1–v1.0: Body parsing
    {"parse_json_body", (PyCFunction)py_parse_json_body, METH_O, nullptr},
    {"serialize_to_json_bytes", (PyCFunction)py_serialize_to_json_bytes, METH_O, nullptr},
    {"serialize_to_json_bytes_pretty", (PyCFunction)py_serialize_to_json_bytes_pretty, METH_O, nullptr},

    // v0.1–v1.0: Content type
    {"parse_content_type", (PyCFunction)py_parse_content_type, METH_O, nullptr},
    {"is_json_content_type", (PyCFunction)py_is_json_content_type, METH_O, nullptr},

    // v0.1–v1.0: Dependency resolver
    {"compute_dependency_order", (PyCFunction)py_compute_dependency_order, METH_O, nullptr},

    // v0.3: Pipeline
    {"encode_to_json_bytes", (PyCFunction)py_encode_to_json_bytes, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"encode_to_json_bytes_pretty", (PyCFunction)py_encode_to_json_bytes_pretty, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"process_request", (PyCFunction)py_process_request, METH_VARARGS | METH_KEYWORDS, nullptr},

    // Form parsing
    {"parse_multipart_body", (PyCFunction)py_parse_multipart_body, METH_VARARGS, nullptr},
    {"parse_urlencoded_body", (PyCFunction)py_parse_urlencoded_body, METH_O, nullptr},

    // Scalar coercion
    {"batch_coerce_scalars", (PyCFunction)py_batch_coerce_scalars, METH_VARARGS, nullptr},

    // Error response
    {"serialize_error_response", (PyCFunction)py_serialize_error_response, METH_O, nullptr},
    {"serialize_error_list", (PyCFunction)py_serialize_error_list, METH_O, nullptr},

    // Param extractor
    {"register_route_params", (PyCFunction)py_register_route_params, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"batch_extract_all_params", (PyCFunction)py_batch_extract_all_params, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"batch_extract_params_inline", (PyCFunction)py_batch_extract_params_inline, METH_VARARGS | METH_KEYWORDS, nullptr},
    {"unregister_route_params", (PyCFunction)py_unregister_route_params, METH_O, nullptr},

    {nullptr, nullptr, 0, nullptr}
};

// ══════════════════════════════════════════════════════════════════════════════
// Module cleanup — release all cached static refs at interpreter shutdown
// ══════════════════════════════════════════════════════════════════════════════

// Extern cleanup functions from other translation units
extern void cleanup_param_registry();
extern void cleanup_dep_plans();

static void module_free(void* /*module*/) {
    // Clean up json_writer.cpp cached type objects
    json_writer_cleanup();

    // Clean up ASGI constants
    cleanup_asgi_constants();

    // Clean up global registries (LEAK-2 fix)
    cleanup_param_registry();
    cleanup_dep_plans();
}

// ══════════════════════════════════════════════════════════════════════════════
// Module definition
// ══════════════════════════════════════════════════════════════════════════════

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_fastapi_core",           // C++ core module
    "FastAPI C++ Core — Direct CPython C API (zero overhead)",
    -1,
    module_methods,
    nullptr,                   // m_slots
    nullptr,                   // m_traverse
    nullptr,                   // m_clear
    (freefunc)module_free,     // m_free — clean up statics on shutdown
};

PyMODINIT_FUNC PyInit__fastapi_core(void) {
    PyObject* m = PyModule_Create(&module_def);
    if (!m) return nullptr;

    // Initialize pre-interned ASGI strings
    if (init_asgi_constants() < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    // Register types: CoreApp, MatchResult, ResponseData, InlineResult
    if (register_app_types(m) < 0) {
        Py_DECREF(m);
        return nullptr;
    }

    // Register CoreRouter type (v0.1 compat)
    if (PyType_Ready(&CoreRouterType) < 0) { Py_DECREF(m); return nullptr; }
    Py_INCREF(&CoreRouterType);
    PyModule_AddObject(m, "CoreRouter", (PyObject*)&CoreRouterType);

    // Pre-initialize JSON writer special type caches
    json_writer_init();

    return m;
}
