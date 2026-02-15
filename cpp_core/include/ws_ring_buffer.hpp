#pragma once
#include <Python.h>
#include <cstddef>
#include <cstring>
#include <utility>

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
    static constexpr size_t CAPACITY = 128 * 1024;  // 128KB
    static constexpr size_t MAX_FRAME_SIZE = 64 * 1024;  // Largest non-fragmented frame

    WsRingBuffer();
    ~WsRingBuffer() = default;

    // Copy/move operations
    WsRingBuffer(const WsRingBuffer&) = delete;
    WsRingBuffer& operator=(const WsRingBuffer&) = delete;
    WsRingBuffer(WsRingBuffer&&) = default;
    WsRingBuffer& operator=(WsRingBuffer&&) = default;

    // ── Core Operations ──────────────────────────────────────────────────

    // Append data from network receive
    // Returns true on success, false if buffer full (caller should apply backpressure)
    bool append(const uint8_t* data, size_t len);

    // Get contiguous readable region for frame parsing
    // May return less than total readable bytes if data wraps around ring end
    // Caller should consume(), then call readable_region() again for wrapped portion
    std::pair<const uint8_t*, size_t> readable_region() const;

    // Mark N bytes as consumed after parsing
    // O(1) operation - just advances read pointer
    void consume(size_t n);

    // ── Capacity & State ─────────────────────────────────────────────────

    // Total bytes available for reading
    size_t readable() const {
        return write_pos_ - read_pos_;
    }

    // Total bytes available for writing (for backpressure detection)
    size_t available() const {
        return CAPACITY - (write_pos_ - read_pos_);
    }

    // Check if buffer is empty
    bool empty() const {
        return read_pos_ == write_pos_;
    }

    // Check if buffer is full
    bool full() const {
        return (write_pos_ - read_pos_) >= CAPACITY;
    }

    // Reset buffer to initial state (for connection close)
    void reset() {
        read_pos_ = 0;
        write_pos_ = 0;
    }

private:
    // Circular buffer storage (cache-line aligned for CPU prefetcher)
    alignas(64) uint8_t buffer_[CAPACITY];

    // Read and write positions (monotonically increasing)
    // Actual index = pos & (CAPACITY - 1)  [fast modulo for power-of-2]
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
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
