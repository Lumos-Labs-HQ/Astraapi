#include "ws_ring_buffer.hpp"
#include "ws_frame_parser.hpp"
#include "json_parser.hpp"
#include "pyref.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <vector>

// ── Platform-specific aligned allocation ─────────────────────────────────
namespace {
uint8_t* alloc_aligned(size_t alignment, size_t size) {
#ifdef _WIN32
    return (uint8_t*)_aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return (uint8_t*)ptr;
#endif
}

void free_aligned(void* ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
} // anonymous namespace

// ── WsRingBuffer Implementation ─────────────────────────────────────────

WsRingBuffer::WsRingBuffer(size_t initial_capacity, size_t max_capacity)
    : max_capacity_(max_capacity)
{
    // Round up to power of 2
    capacity_ = 1;
    while (capacity_ < initial_capacity) capacity_ <<= 1;
    if (capacity_ > max_capacity_) capacity_ = max_capacity_;

    buffer_ = alloc_aligned(64, capacity_);
    scratch_ = alloc_aligned(64, capacity_);
    scratch_cap_ = capacity_;
}

WsRingBuffer::~WsRingBuffer() {
    free_aligned(buffer_);
    free_aligned(scratch_);
}

bool WsRingBuffer::grow(size_t required) {
    size_t needed = readable() + required;
    size_t new_cap = capacity_;
    while (new_cap < needed) {
        new_cap <<= 1;
    }
    if (new_cap > max_capacity_) return false;
    if (new_cap <= capacity_) return true;  // already big enough

    uint8_t* new_buf = alloc_aligned(64, new_cap);
    if (!new_buf) return false;

    // Copy existing data contiguously into new buffer
    size_t r = readable();
    if (r > 0) {
        size_t read_idx = read_pos_ & (capacity_ - 1);
        size_t first = capacity_ - read_idx;
        if (first >= r) {
            memcpy(new_buf, buffer_ + read_idx, r);
        } else {
            memcpy(new_buf, buffer_ + read_idx, first);
            memcpy(new_buf + first, buffer_, r - first);
        }
    }

    free_aligned(buffer_);
    buffer_ = new_buf;
    capacity_ = new_cap;
    read_pos_ = 0;
    write_pos_ = r;

    // Grow scratch buffer to match
    if (scratch_cap_ < new_cap) {
        free_aligned(scratch_);
        scratch_ = alloc_aligned(64, new_cap);
        scratch_cap_ = new_cap;
    }

    return true;
}

bool WsRingBuffer::append(const uint8_t* data, size_t len) {
    if (len == 0) return true;

    // Try to grow if not enough space
    if (len > available()) {
        if (!grow(len)) {
            return false;  // Max capacity exceeded — caller should apply backpressure
        }
    }

    // Calculate actual buffer indices (fast modulo for power-of-2)
    size_t write_idx = write_pos_ & (capacity_ - 1);
    size_t bytes_to_end = capacity_ - write_idx;

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
    size_t read_idx = read_pos_ & (capacity_ - 1);
    size_t write_idx = write_pos_ & (capacity_ - 1);

    // Return contiguous region (may be less than total readable if wrapped)
    if (write_idx > read_idx) {
        // No wrap: return [read_idx, write_idx)
        return {buffer_ + read_idx, write_idx - read_idx};
    } else {
        // Wrapped: return [read_idx, capacity_) first
        // Caller must consume() then call readable_region() again for [0, write_idx)
        return {buffer_ + read_idx, capacity_ - read_idx};
    }
}

void WsRingBuffer::consume(size_t n) {
    assert(n <= readable() && "Cannot consume more bytes than available");
    read_pos_ += n;

    // Optimization: reset positions when buffer is empty to prevent overflow
    if (read_pos_ == write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

std::pair<uint8_t*, size_t> WsRingBuffer::readable_contiguous() {
    if (empty()) return {nullptr, 0};

    size_t read_idx = read_pos_ & (capacity_ - 1);
    size_t write_idx = write_pos_ & (capacity_ - 1);
    size_t total = readable();

    if (write_idx > read_idx) {
        // Not wrapped — return direct mutable pointer (zero-copy)
        return {buffer_ + read_idx, total};
    }

    // Wrapped — copy both segments into instance-owned scratch buffer
    size_t first = capacity_ - read_idx;
    memcpy(scratch_, buffer_ + read_idx, first);
    memcpy(scratch_ + first, buffer_, write_idx);
    return {scratch_, total};
}

// ── Python C API Integration ─────────────────────────────────────────────

static const char* WS_CAPSULE_NAME = "ws_connection_state";

namespace {

// Destructor for PyCapsule
void ws_connection_state_destructor(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, WS_CAPSULE_NAME);
    if (ptr) {
        delete static_cast<WsConnectionState*>(ptr);
    }
}

// Helper to extract WsConnectionState from capsule
WsConnectionState* get_conn_state(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, WS_CAPSULE_NAME);
    return static_cast<WsConnectionState*>(ptr);
}

} // anonymous namespace

PyObject* py_ws_ring_buffer_create(PyObject* /*self*/, PyObject* /*args*/) {
    WsConnectionState* state = new WsConnectionState();
    return PyCapsule_New(state, WS_CAPSULE_NAME, ws_connection_state_destructor);
}

PyObject* py_ws_ring_buffer_append(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;

    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    bool success = state->ring.append(static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
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

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    auto [data, size] = state->ring.readable_region();

    if (data == nullptr) {
        Py_RETURN_NONE;
    }

    // Return a safe copy as PyBytes. This function is NOT on the hot path
    // (the *_direct handlers use readable_contiguous() internally).
    // Returning a copy prevents use-after-free if Python holds the reference
    // across consume() calls.
    return PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(data),
        static_cast<Py_ssize_t>(size)
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

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    if (static_cast<size_t>(n) > state->ring.readable()) {
        PyErr_SetString(PyExc_ValueError, "Cannot consume more bytes than available");
        return nullptr;
    }

    state->ring.consume(static_cast<size_t>(n));
    Py_RETURN_NONE;
}

PyObject* py_ws_ring_buffer_readable(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;

    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    return PyLong_FromSize_t(state->ring.readable());
}

PyObject* py_ws_ring_buffer_reset(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;

    if (!PyArg_ParseTuple(args, "O", &capsule)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    state->reset();
    Py_RETURN_NONE;
}

// ── ws_echo_direct: Single-call echo (ring buffer + parse + echo + consume) ──

PyObject* py_ws_echo_direct(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;

    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    // Step 1: Append incoming data to ring buffer
    bool appended = state->ring.append(
        static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
    PyBuffer_Release(&py_buf);

    if (!appended) {
        // Ring buffer full — signal backpressure
        Py_RETURN_NONE;
    }

    // Step 2: Get contiguous readable data (handles wrap-around)
    auto [data, data_len] = state->ring.readable_contiguous();
    if (data == nullptr || data_len == 0) {
        // No complete data yet
        return Py_BuildValue("(OO)", Py_None, Py_None);
    }

    // Step 3: Parse + unmask + build echo (GIL released)
    static thread_local std::vector<uint8_t> out;
    out.clear();
    if (out.capacity() < data_len + 64) {
        out.reserve((data_len + 64) * 2);
    }

    EchoResult eres;

    Py_BEGIN_ALLOW_THREADS
    ws_echo_frames_nogil(data, data_len, out, eres);
    Py_END_ALLOW_THREADS

    // Step 4: Build Python result objects (before consume, data still valid)
    PyObject* echo_bytes;
    if (!out.empty()) {
        echo_bytes = PyBytes_FromStringAndSize(
            (const char*)out.data(), (Py_ssize_t)out.size());
        if (!echo_bytes) return nullptr;
    } else {
        Py_INCREF(Py_None);
        echo_bytes = Py_None;
    }

    PyObject* close_obj;
    if (eres.has_close) {
        close_obj = PyBytes_FromStringAndSize(
            (const char*)(data + eres.close_payload_offset),
            (Py_ssize_t)eres.close_payload_len);
        if (!close_obj) {
            Py_DECREF(echo_bytes);
            return nullptr;
        }
    } else {
        Py_INCREF(Py_None);
        close_obj = Py_None;
    }

    // Step 5: Consume processed bytes from ring buffer (O(1))
    if (eres.total_consumed > 0) {
        state->ring.consume(eres.total_consumed);
    }

    // Return (echo_bytes, close_payload)
    PyObject* result = PyTuple_Pack(2, echo_bytes, close_obj);
    Py_DECREF(echo_bytes);
    Py_DECREF(close_obj);
    return result;
}

// ── ws_handle_direct: Single-call text/binary handler ───────────────────────
// Replaces the entire _handle_ws_frames Python method (~210 lines) with one C++ call.
// ring_buffer_append + parse + unmask (GIL released) + UTF-8 decode + consume

PyObject* py_ws_handle_direct(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;

    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    // Step 1: Append incoming data to ring buffer
    bool appended = state->ring.append(
        static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
    PyBuffer_Release(&py_buf);

    if (!appended) {
        Py_RETURN_NONE;  // Ring buffer full — backpressure
    }

    // Step 2: Get contiguous readable data (handles wrap-around)
    auto [data, data_len] = state->ring.readable_contiguous();
    if (data == nullptr || data_len == 0) {
        Py_RETURN_NONE;  // Need more data
    }

    // Step 3: Parse + unmask all frames (GIL released), with fragment assembly
    ParseResult pres;

    Py_BEGIN_ALLOW_THREADS
    ws_parse_frames_nogil(data, data_len, pres, &state->assembler);
    Py_END_ALLOW_THREADS

    // Check for protocol errors — signal close with code 1002
    if (pres.protocol_error != 0) {
        // Build a synthetic close frame to tell Python layer to close
        uint8_t close_payload[2] = { 0x03, 0xEA };  // 1002 = Protocol Error
        PyRef frames(PyList_New(1));
        if (!frames) return nullptr;
        PyObject* payload_obj = PyBytes_FromStringAndSize(
            (const char*)close_payload, 2);
        if (!payload_obj) return nullptr;
        PyObject* opcode_obj = PyLong_FromLong(WS_CLOSE);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }
        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;
        PyList_SET_ITEM(frames.get(), 0, tuple);
        Py_INCREF(Py_None);
        PyObject* result = PyTuple_Pack(2, frames.get(), Py_None);
        Py_DECREF(Py_None);
        return result;
    }

    if (pres.total_consumed == 0 && pres.frames.empty() && pres.assembled.empty()) {
        Py_RETURN_NONE;  // Need more data
    }

    // Step 4: Build Python frame list (GIL held)
    // Total frames = inline frames + assembled messages
    size_t total_frames = pres.frames.size() + pres.assembled.size();
    PyRef frames(PyList_New((Py_ssize_t)total_frames));
    if (!frames) return nullptr;

    Py_ssize_t idx = 0;

    // Add inline (single-frame) messages
    for (size_t i = 0; i < pres.frames.size(); i++) {
        const auto& fr = pres.frames[i];
        const char* payload_ptr = (const char*)(data + fr.payload_offset);

        PyObject* payload_obj;
        if (fr.opcode == WS_TEXT) {
            payload_obj = PyUnicode_DecodeUTF8(
                payload_ptr, (Py_ssize_t)fr.payload_len, "surrogateescape");
            if (!payload_obj) {
                PyErr_Clear();
                payload_obj = PyBytes_FromStringAndSize(
                    payload_ptr, (Py_ssize_t)fr.payload_len);
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(
                payload_ptr, (Py_ssize_t)fr.payload_len);
        }
        if (!payload_obj) return nullptr;

        PyObject* opcode_obj = PyLong_FromLong(fr.opcode);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;

        PyList_SET_ITEM(frames.get(), idx++, tuple);
    }

    // Add assembled (multi-fragment) messages
    for (size_t i = 0; i < pres.assembled.size(); i++) {
        const auto& msg = pres.assembled[i];
        const char* payload_ptr = (const char*)msg.payload.data();
        Py_ssize_t payload_len = (Py_ssize_t)msg.payload.size();

        PyObject* payload_obj;
        if (msg.opcode == WS_TEXT) {
            payload_obj = PyUnicode_DecodeUTF8(
                payload_ptr, payload_len, "surrogateescape");
            if (!payload_obj) {
                PyErr_Clear();
                payload_obj = PyBytes_FromStringAndSize(payload_ptr, payload_len);
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(payload_ptr, payload_len);
        }
        if (!payload_obj) return nullptr;

        PyObject* opcode_obj = PyLong_FromLong(msg.opcode);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;

        PyList_SET_ITEM(frames.get(), idx++, tuple);
    }

    // Step 5: Consume processed bytes from ring buffer
    if (pres.total_consumed > 0) {
        state->ring.consume(pres.total_consumed);
    }

    // Step 6: Build pong bytes (if any PING frames were received)
    PyObject* py_pong;
    if (!pres.pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize(
            (const char*)pres.pong_buf.data(), (Py_ssize_t)pres.pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    // Return (frames_list, pong_bytes_or_none)
    PyObject* result = PyTuple_Pack(2, frames.get(), py_pong);
    Py_DECREF(py_pong);
    return result;
}

// ── ws_handle_json_direct: Single-call JSON handler ─────────────────────────
// Like ws_handle_direct but TEXT frame payloads are JSON-parsed via yyjson.
// Fixes GIL bug: old py_ws_parse_frames_json held GIL during parse+unmask.

PyObject* py_ws_handle_json_direct(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;

    if (!PyArg_ParseTuple(args, "Oy*", &capsule, &py_buf)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyBuffer_Release(&py_buf);
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    // Step 1: Append incoming data to ring buffer
    bool appended = state->ring.append(
        static_cast<const uint8_t*>(py_buf.buf), py_buf.len);
    PyBuffer_Release(&py_buf);

    if (!appended) {
        Py_RETURN_NONE;  // Ring buffer full — backpressure
    }

    // Step 2: Get contiguous readable data
    auto [data, data_len] = state->ring.readable_contiguous();
    if (data == nullptr || data_len == 0) {
        Py_RETURN_NONE;
    }

    // Step 3: Parse + unmask all frames (GIL released), with fragment assembly
    ParseResult pres;

    Py_BEGIN_ALLOW_THREADS
    ws_parse_frames_nogil(data, data_len, pres, &state->assembler);
    Py_END_ALLOW_THREADS

    // Check for protocol errors
    if (pres.protocol_error != 0) {
        uint8_t close_payload[2] = { 0x03, 0xEA };  // 1002
        PyRef frames(PyList_New(1));
        if (!frames) return nullptr;
        PyObject* payload_obj = PyBytes_FromStringAndSize(
            (const char*)close_payload, 2);
        if (!payload_obj) return nullptr;
        PyObject* opcode_obj = PyLong_FromLong(WS_CLOSE);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }
        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;
        PyList_SET_ITEM(frames.get(), 0, tuple);
        Py_INCREF(Py_None);
        PyObject* result = PyTuple_Pack(2, frames.get(), Py_None);
        Py_DECREF(Py_None);
        return result;
    }

    if (pres.total_consumed == 0 && pres.frames.empty() && pres.assembled.empty()) {
        Py_RETURN_NONE;
    }

    // Step 4: Build Python frame list with JSON decode for TEXT frames
    size_t total_frames = pres.frames.size() + pres.assembled.size();
    PyRef frames(PyList_New((Py_ssize_t)total_frames));
    if (!frames) return nullptr;

    Py_ssize_t idx = 0;

    // Inline (single-frame) messages
    for (size_t i = 0; i < pres.frames.size(); i++) {
        const auto& fr = pres.frames[i];
        const char* payload_ptr = (const char*)(data + fr.payload_offset);

        PyObject* payload_obj;
        if (fr.opcode == WS_TEXT && fr.payload_len > 0) {
            payload_obj = yyjson_parse_to_pyobject(
                payload_ptr, (size_t)fr.payload_len);
            if (!payload_obj) {
                PyErr_Clear();
                payload_obj = PyUnicode_DecodeUTF8(
                    payload_ptr, (Py_ssize_t)fr.payload_len, "surrogateescape");
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(
                payload_ptr, (Py_ssize_t)fr.payload_len);
        }
        if (!payload_obj) return nullptr;

        PyObject* opcode_obj = PyLong_FromLong(fr.opcode);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;

        PyList_SET_ITEM(frames.get(), idx++, tuple);
    }

    // Assembled (multi-fragment) messages
    for (size_t i = 0; i < pres.assembled.size(); i++) {
        const auto& msg = pres.assembled[i];
        const char* payload_ptr = (const char*)msg.payload.data();
        Py_ssize_t payload_len = (Py_ssize_t)msg.payload.size();

        PyObject* payload_obj;
        if (msg.opcode == WS_TEXT && payload_len > 0) {
            payload_obj = yyjson_parse_to_pyobject(
                payload_ptr, (size_t)payload_len);
            if (!payload_obj) {
                PyErr_Clear();
                payload_obj = PyUnicode_DecodeUTF8(
                    payload_ptr, payload_len, "surrogateescape");
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(payload_ptr, payload_len);
        }
        if (!payload_obj) return nullptr;

        PyObject* opcode_obj = PyLong_FromLong(msg.opcode);
        if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) return nullptr;

        PyList_SET_ITEM(frames.get(), idx++, tuple);
    }

    // Step 5: Consume processed bytes
    if (pres.total_consumed > 0) {
        state->ring.consume(pres.total_consumed);
    }

    // Step 6: Build pong bytes
    PyObject* py_pong;
    if (!pres.pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize(
            (const char*)pres.pong_buf.data(), (Py_ssize_t)pres.pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    PyObject* result = PyTuple_Pack(2, frames.get(), py_pong);
    Py_DECREF(py_pong);
    return result;
}
