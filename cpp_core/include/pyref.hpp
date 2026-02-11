#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <utility>

// RAII wrapper for PyObject* — prevents reference leaks.
// Auto Py_XDECREF on destruction, move-only semantics.
class PyRef {
    PyObject* ptr_;
public:
    // Steal a new reference (default)
    explicit PyRef(PyObject* p = nullptr) noexcept : ptr_(p) {}

    ~PyRef() { Py_XDECREF(ptr_); }

    // Move only — no copy
    PyRef(PyRef&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    PyRef& operator=(PyRef&& o) noexcept {
        if (this != &o) { Py_XDECREF(ptr_); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }
    PyRef(const PyRef&) = delete;
    PyRef& operator=(const PyRef&) = delete;

    PyObject* get() const noexcept { return ptr_; }
    PyObject* release() noexcept { auto p = ptr_; ptr_ = nullptr; return p; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Borrow a reference (increfs)
    static PyRef borrow(PyObject* p) {
        Py_XINCREF(p);
        return PyRef(p);
    }
};

// Helper: set error and return NULL
inline PyObject* set_error(PyObject* exc_type, const char* msg) {
    PyErr_SetString(exc_type, msg);
    return nullptr;
}
