#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "buffer_pool.hpp"

static thread_local std::vector<std::vector<char>> pool;

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
    if (pool.size() < BUFFER_POOL_MAX) {
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
    for (int i = 0; i < count && pool.size() < BUFFER_POOL_MAX; i++) {
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
