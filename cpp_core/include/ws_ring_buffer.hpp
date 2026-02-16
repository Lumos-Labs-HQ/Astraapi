#pragma once
#include <Python.h>
#include <cstddef>
#include <cstring>
#include <utility>
#include "ws_frame_parser.hpp"

// WsRingBuffer — Circular buffer for WebSocket frame accumulation
//
// Replaces bytearray + offset tracking in Python to eliminate expensive
// memmove operations when offset > 64KB (see _cpp_server.py:695).
//
// Key features:
// - 128KB capacity (power of 2 for efficient modulo)
// - O(1) append and consume operations (vs O(N) memmove)
// - Cache-line aligned for optimal CPU prefetching
// - Zero-copy readable_region() returns view into buffer
// - Handles wrap-around automatically

class WsRingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 128 * 1024;  // 128KB initial
    static constexpr size_t MAX_CAPACITY = 16 * 1024 * 1024;  // 16MB hard limit

    explicit WsRingBuffer(size_t initial_capacity = DEFAULT_CAPACITY,
                          size_t max_capacity = MAX_CAPACITY);
    ~WsRingBuffer();

    // Copy/move operations
    WsRingBuffer(const WsRingBuffer&) = delete;
    WsRingBuffer& operator=(const WsRingBuffer&) = delete;
    WsRingBuffer(WsRingBuffer&&) = delete;
    WsRingBuffer& operator=(WsRingBuffer&&) = delete;

    // ── Core Operations ──────────────────────────────────────────────────

    // Append data from network receive
    // Grows buffer if needed (up to max_capacity). Returns false only if max exceeded.
    bool append(const uint8_t* data, size_t len);

    // Get contiguous readable region for frame parsing
    // May return less than total readable bytes if data wraps around ring end
    // Caller should consume(), then call readable_region() again for wrapped portion
    std::pair<const uint8_t*, size_t> readable_region() const;

    // Mark N bytes as consumed after parsing
    // O(1) operation - just advances read pointer
    void consume(size_t n);

    // Get contiguous mutable view of ALL readable data.
    // Zero-copy if data doesn't wrap. If wrapped, linearizes in-place.
    // Used by ws_echo_direct() to avoid memoryview creation overhead.
    std::pair<uint8_t*, size_t> readable_contiguous();

    // ── Capacity & State ─────────────────────────────────────────────────

    // Total bytes available for reading
    size_t readable() const {
        return write_pos_ - read_pos_;
    }

    // Total bytes available for writing before growth is needed
    size_t available() const {
        return capacity_ - (write_pos_ - read_pos_);
    }

    // Current capacity
    size_t capacity() const { return capacity_; }

    // Check if buffer is empty
    bool empty() const {
        return read_pos_ == write_pos_;
    }

    // Check if buffer is full (at current capacity, may still grow)
    bool full() const {
        return (write_pos_ - read_pos_) >= capacity_;
    }

    // Reset buffer to initial state (for connection close/reuse)
    void reset() {
        read_pos_ = 0;
        write_pos_ = 0;
        // Don't shrink — keep allocated capacity for reuse
    }

private:
    // Try to grow buffer. Returns true on success.
    bool grow(size_t required);

    // Heap-allocated circular buffer (cache-line aligned)
    uint8_t* buffer_;
    size_t capacity_;       // current capacity (always power of 2)
    size_t max_capacity_;

    // Read and write positions (monotonically increasing)
    // Actual index = pos & (capacity_ - 1)  [fast modulo for power-of-2]
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
};

// WsConnectionState — bundles ring buffer + fragment assembler per connection
struct WsConnectionState {
    WsRingBuffer ring;
    WsFragmentAssembler assembler;

    void reset() {
        ring.reset();
        assembler.reset();
    }
};

// ── Python C API Integration ─────────────────────────────────────────────

// Python-facing functions (exported via module.cpp)

// Create new ring buffer (returns PyCapsule wrapping WsRingBuffer*)
PyObject* py_ws_ring_buffer_create(PyObject* self, PyObject* args);

// Append data to ring buffer
// Args: (capsule, bytes) → Returns: bool (success)
PyObject* py_ws_ring_buffer_append(PyObject* self, PyObject* args);

// Get readable region as memoryview
// Args: (capsule,) → Returns: memoryview or None
PyObject* py_ws_ring_buffer_readable_region(PyObject* self, PyObject* args);

// Consume N bytes
// Args: (capsule, int) → Returns: None
PyObject* py_ws_ring_buffer_consume(PyObject* self, PyObject* args);

// Get readable byte count
// Args: (capsule,) → Returns: int
PyObject* py_ws_ring_buffer_readable(PyObject* self, PyObject* args);

// Reset buffer
// Args: (capsule,) → Returns: None
PyObject* py_ws_ring_buffer_reset(PyObject* self, PyObject* args);

// Single-call echo: append data to ring buffer + parse + unmask + build echo + consume
// Args: (capsule, bytes) → Returns: (echo_bytes|None, close_payload|None) or None (backpressure)
PyObject* py_ws_echo_direct(PyObject* self, PyObject* args);

// Ultra-fast echo: parses + builds echo + writes directly to socket FD (bypasses asyncio)
// Args: (capsule, bytes, fd) → Returns: None | close_payload bytes
PyObject* py_ws_echo_direct_fd(PyObject* self, PyObject* args);

// Single-call text/binary handler: append + parse + unmask (GIL released) + UTF-8 decode + consume
// Args: (capsule, bytes) → Returns: ([(opcode, str|bytes), ...], pong_bytes|None) or None
PyObject* py_ws_handle_direct(PyObject* self, PyObject* args);

// Single-call JSON handler: append + parse + unmask (GIL released) + JSON decode + consume
// Args: (capsule, bytes) → Returns: ([(opcode, parsed_obj|bytes), ...], pong_bytes|None) or None
PyObject* py_ws_handle_json_direct(PyObject* self, PyObject* args);
