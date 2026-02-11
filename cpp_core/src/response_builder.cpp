#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "asgi_constants.hpp"
#include "json_writer.hpp"
#include "buffer_pool.hpp"
#include "pyref.hpp"
#include <cstring>
#include <cstdio>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Internal response building helpers (used by app.cpp via json_writer.hpp)
// ══════════════════════════════════════════════════════════════════════════════
//
// The main response building is done in CoreApp.build_asgi_response (app.cpp).
// This file provides module-level build_* helpers for direct ResponseData construction.
//
// Since ResponseData is a PyTypeObject in app.hpp/app.cpp, this file provides
// helper utilities that can be used if needed.
// ══════════════════════════════════════════════════════════════════════════════

// Placeholder: response_builder functionality is integrated into app.cpp
// (build_asgi_response method) and json_writer.cpp (serialize_to_json_pybytes).
// This file exists to satisfy the CMakeLists.txt source list.
