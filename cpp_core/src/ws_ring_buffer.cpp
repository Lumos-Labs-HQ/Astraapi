#include "ws_ring_buffer.hpp"
#include "pyref.hpp"
#include <algorithm>
#include <cassert>

// ── WsRingBuffer Implementation ─────────────────────────────────────────

WsRingBuffer::WsRingBuffer() {
    // Zero-initialize buffer (optional, for security)
    // memset(buffer_, 0, CAPACITY);  // Can skip for performance
}

bool WsRingBuffer::append(const uint8_t* data, size_t len) {
    if (len == 0) return true;

    // Check capacity
    if (len > available()) {
        return false;  // Buffer full - caller should apply backpressure
    }

    // Calculate actual buffer indices (fast modulo for power-of-2)
    size_t write_idx = write_pos_ & (CAPACITY - 1);
    size_t bytes_to_end = CAPACITY - write_idx;

    if (len <= bytes_to_end) {
        // Data fits before wrap - single memcpy
        memcpy(buffer_ + write_idx, data, len);
    } else {
        // Data wraps around - two memcpys
        memcpy(buffer_ + write_idx, data, bytes_to_end);
        memcpy(buffer_, data + bytes_to_end, len - bytes_to_end);
    }

    write_pos_ += len;
    return true;
}

std::pair<const uint8_t*, size_t> WsRingBuffer::readable_region() const {
    if (empty()) {
        return {nullptr, 0};
    }

    // Calculate actual buffer indices
    size_t read_idx = read_pos_ & (CAPACITY - 1);
    size_t write_idx = write_pos_ & (CAPACITY - 1);

    // Return contiguous region (may be less than total readable if wrapped)
    if (write_idx > read_idx) {
        // No wrap: return [read_idx, write_idx)
        return {buffer_ + read_idx, write_idx - read_idx};
    } else {
        // Wrapped: return [read_idx, CAPACITY) first
        // Caller must consume() then call readable_region() again for [0, write_idx)
        return {buffer_ + read_idx, CAPACITY - read_idx};
    }
}

void WsRingBuffer::consume(size_t n) {
    assert(n <= readable() && "Cannot consume more bytes than available");
    read_pos_ += n;

    // Optimization: reset positions when buffer is empty to prevent overflow
    // (monotonic counters would overflow after ~584 billion years at 1GB/s, but still...)
    if (read_pos_ == write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

// ── Python C API Integration ─────────────────────────────────────────────

namespace {

// Destructor for PyCapsule
void ws_ring_buffer_destructor(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, "ws_ring_buffer");
    if (ptr) {
        delete static_cast<WsRingBuffer*>(ptr);
    }
}

} // anonymous namespace

PyObject* py_ws_ring_buffer_create(PyObject* /*self*/, PyObject* /*args*/) {
    WsRingBuffer* buf = new WsRingBuffer();
    return PyCapsule_New(buf, "ws_ring_buffer", ws_ring_buffer_destructor);
}

PyObject* py_ws_ring_buffer_append(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;

    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) {
        return nullptr;
    }

    WsRingBuffer* ring = static_cast<WsRingBuffer*>(
        PyCapsule_GetPointer(capsule, "ws_ring_buffer"));
    if (!ring) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid ws_ring_buffer capsule");
        return nullptr;
    }

    bool success = ring->append(static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
    PyBuffer_Release(&py_buf);

    if (success) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

PyObject* py_ws_ring_buffer_readable_region(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;

    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return nullptr;
    }

    WsRingBuffer* ring = static_cast<WsRingBuffer*>(
        PyCapsule_GetPointer(capsule, "ws_ring_buffer"));
    if (!ring) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_ring_buffer capsule");
        return nullptr;
    }

    auto [data, size] = ring->readable_region();

    if (data == nullptr) {
        Py_RETURN_NONE;
    }

    // Return memoryview (zero-copy view into ring buffer)
    // IMPORTANT: Python must not hold this reference across consume() calls
    // as the underlying buffer is reused
    return PyMemoryView_FromMemory(
        const_cast<char*>(reinterpret_cast<const char*>(data)),
        static_cast<Py_ssize_t>(size),
        PyBUF_READ
    );
}

PyObject* py_ws_ring_buffer_consume(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_ssize_t n;

    if (!PyArg_ParseTuple(args, "On", &capsule, &n)) {
        return nullptr;
    }

    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "Cannot consume negative bytes");
        return nullptr;
    }

    WsRingBuffer* ring = static_cast<WsRingBuffer*>(
        PyCapsule_GetPointer(capsule, "ws_ring_buffer"));
    if (!ring) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_ring_buffer capsule");
        return nullptr;
    }

    if (static_cast<size_t>(n) > ring->readable()) {
        PyErr_SetString(PyExc_ValueError, "Cannot consume more bytes than available");
        return nullptr;
    }

    ring->consume(static_cast<size_t>(n));
    Py_RETURN_NONE;
}

PyObject* py_ws_ring_buffer_readable(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;

    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return nullptr;
    }

    WsRingBuffer* ring = static_cast<WsRingBuffer*>(
        PyCapsule_GetPointer(capsule, "ws_ring_buffer"));
    if (!ring) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_ring_buffer capsule");
        return nullptr;
    }

    return PyLong_FromSize_t(ring->readable());
}

PyObject* py_ws_ring_buffer_reset(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;

    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return nullptr;
    }

    WsRingBuffer* ring = static_cast<WsRingBuffer*>(
        PyCapsule_GetPointer(capsule, "ws_ring_buffer"));
    if (!ring) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_ring_buffer capsule");
        return nullptr;
    }

    ring->reset();
    Py_RETURN_NONE;
}
