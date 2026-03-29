#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "buffer_pool.hpp"

static thread_local std::vector<std::vector<char>> pool;
static thread_local size_t tl_pool_max = BUFFER_POOL_MAX;

void set_buffer_pool_max(size_t n) {
    tl_pool_max = (n > 0) ? n : 1;
}

std::vector<char> acquire_buffer() {
    if (!pool.empty()) {
        auto buf = std::move(pool.back());
        pool.pop_back();
        buf.clear();
        return buf;
    }
    std::vector<char> buf;
    buf.reserve(BUFFER_INITIAL_CAPACITY);
    return buf;
}

void release_buffer(std::vector<char> buf) {
    if (pool.size() < tl_pool_max) {
        if (buf.capacity() > BUFFER_INITIAL_CAPACITY * 4) {
            // Replace oversized buffer with a fresh right-sized one (single alloc)
            std::vector<char> fresh;
            fresh.reserve(BUFFER_INITIAL_CAPACITY);
            pool.push_back(std::move(fresh));
            return;  // drop oversized buf
        }
        buf.clear();
        pool.push_back(std::move(buf));
    }
    // else: buf is dropped (freed) — pool is full
}

void prewarm_buffer_pool(int count) {
    for (int i = 0; i < count && pool.size() < tl_pool_max; i++) {
        std::vector<char> buf;
        buf.reserve(BUFFER_INITIAL_CAPACITY);
        pool.push_back(std::move(buf));
    }
}

// Python-callable wrapper: prewarm_buffer_pool(count=4)
PyObject* py_prewarm_buffer_pool(PyObject* /*self*/, PyObject* args) {
    int count = 4;
    if (!PyArg_ParseTuple(args, "|i", &count)) return nullptr;
    prewarm_buffer_pool(count);
    Py_RETURN_NONE;
}

PyObject* py_set_buffer_pool_max(PyObject* /*self*/, PyObject* args) {
    int n = 0;
    if (!PyArg_ParseTuple(args, "i", &n)) return nullptr;
    if (n <= 0) {
        PyErr_SetString(PyExc_ValueError, "buffer pool max must be > 0");
        return nullptr;
    }
    set_buffer_pool_max((size_t)n);
    Py_RETURN_NONE;
}
