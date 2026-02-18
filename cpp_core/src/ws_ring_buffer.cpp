#include "ws_ring_buffer.hpp"
#include "ws_frame_parser.hpp"
#include "json_parser.hpp"
#include "pyref.hpp"
#include <cassert>
#include <cstdlib>
#include <vector>
#include "platform.hpp"   // platform_socket_write(), ssize_t

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

// ── Pre-cached opcode PyLong objects ────────────────────────────────────────
// CPython caches small integers -5..256, so PyLong_FromLong(1) is already cached.
// But PyTuple_Pack(2, ...) has varargs overhead. Using PyTuple_New + SET_ITEM is faster.
static PyObject* g_opcode_text = nullptr;    // 1 (WS_TEXT)
static PyObject* g_opcode_binary = nullptr;  // 2 (WS_BINARY)
static PyObject* g_opcode_close = nullptr;   // 8 (WS_CLOSE)
static PyObject* g_str_done = nullptr;       // interned "done" for waiter.done() calls

void init_ws_opcode_cache() {
    g_opcode_text = PyLong_FromLong(WS_TEXT);       // borrowed from small int cache
    g_opcode_binary = PyLong_FromLong(WS_BINARY);
    g_opcode_close = PyLong_FromLong(WS_CLOSE);
    g_str_done = PyUnicode_InternFromString("done");
}

// Helper: build (opcode, payload) tuple using cached opcodes, avoiding PyTuple_Pack varargs
static inline PyObject* make_frame_tuple(uint8_t opcode, PyObject* payload_obj) {
    PyObject* opcode_obj;
    switch (opcode) {
        case WS_TEXT:   opcode_obj = g_opcode_text; break;
        case WS_BINARY: opcode_obj = g_opcode_binary; break;
        case WS_CLOSE:  opcode_obj = g_opcode_close; break;
        default:        opcode_obj = PyLong_FromLong(opcode);
                         if (!opcode_obj) { Py_DECREF(payload_obj); return nullptr; }
                         break;
    }

    PyObject* tuple = PyTuple_New(2);
    if (!tuple) {
        if (opcode != WS_TEXT && opcode != WS_BINARY && opcode != WS_CLOSE)
            Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        return nullptr;
    }
    Py_INCREF(opcode_obj);
    PyTuple_SET_ITEM(tuple, 0, opcode_obj);
    PyTuple_SET_ITEM(tuple, 1, payload_obj);  // steals ref
    return tuple;
}

// ── WsRingBuffer Implementation ─────────────────────────────────────────

WsRingBuffer::WsRingBuffer(size_t initial_capacity, size_t max_capacity)
    : max_capacity_(max_capacity)
{
    // Round up to power of 2
    capacity_ = 1;
    while (capacity_ < initial_capacity) capacity_ <<= 1;
    if (capacity_ > max_capacity_) capacity_ = max_capacity_;

    buffer_ = alloc_aligned(64, capacity_);
}

WsRingBuffer::~WsRingBuffer() {
    free_aligned(buffer_);
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

void WsRingBuffer::maybe_shrink() {
    // Shrink if capacity grew far beyond current usage to reclaim memory.
    // Only shrink when usage drops below 25% of capacity AND capacity is
    // at least 4x the default. Avoids thrashing for small buffers.
    if (capacity_ <= DEFAULT_CAPACITY) return;
    size_t used = readable();
    if (used >= capacity_ / 4) return;

    // Target: next power of 2 >= max(used*2, DEFAULT_CAPACITY)
    size_t target = (used * 2 > DEFAULT_CAPACITY) ? used * 2 : DEFAULT_CAPACITY;
    size_t new_cap = DEFAULT_CAPACITY;
    while (new_cap < target) new_cap <<= 1;
    if (new_cap >= capacity_) return;

    uint8_t* new_buf = alloc_aligned(64, new_cap);
    if (!new_buf) return;

    // Copy existing data contiguously into new buffer
    if (used > 0) {
        size_t read_idx = read_pos_ & (capacity_ - 1);
        size_t first = capacity_ - read_idx;
        if (first >= used) {
            memcpy(new_buf, buffer_ + read_idx, used);
        } else {
            memcpy(new_buf, buffer_ + read_idx, first);
            memcpy(new_buf + first, buffer_, used - first);
        }
    }

    free_aligned(buffer_);
    buffer_ = new_buf;
    capacity_ = new_cap;
    read_pos_ = 0;
    write_pos_ = used;
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

    // Wrapped — linearize in-place: move data to start of buffer.
    // Data layout: [tail(0..write_idx) | ...gap... | head(read_idx..capacity_)]
    // Goal:        [head | tail | ...gap...]
    //
    // Thread-local scratch buffer: grows to peak usage once, reused across calls.
    // Eliminates per-message malloc/free in the wrap-around case.
    static thread_local std::vector<uint8_t> scratch;

    size_t first = capacity_ - read_idx;  // head segment size
    size_t smaller = (write_idx <= first) ? write_idx : first;

    if (scratch.size() < smaller) scratch.resize(smaller);

    if (write_idx <= first) {
        // Tail is smaller — save tail, move head, append tail
        memcpy(scratch.data(), buffer_, write_idx);
        memmove(buffer_, buffer_ + read_idx, first);
        memcpy(buffer_ + first, scratch.data(), write_idx);
    } else {
        // Head is smaller — save head, move tail, prepend head
        memcpy(scratch.data(), buffer_ + read_idx, first);
        memmove(buffer_ + first, buffer_, write_idx);
        memcpy(buffer_, scratch.data(), first);
    }

    read_pos_ = 0;
    write_pos_ = total;

    // Shrink thread-local scratch if it grew too large (> 64KB)
    if (scratch.capacity() > 65536) {
        scratch.clear();
        scratch.shrink_to_fit();
    }

    return {buffer_, total};
}

// ── Slab allocator for WsConnectionState ─────────────────────────────────
// Pre-allocates a pool of WsConnectionState objects and uses a free-list
// for O(1) acquire/release. Better cache locality and zero malloc overhead
// per connection. Falls back to new/delete when pool is exhausted.

namespace {

class WsConnectionSlab {
    static constexpr size_t SLAB_SIZE = 64;  // pre-allocate 64 states

    // Pool of pre-allocated states
    WsConnectionState slab_[SLAB_SIZE];
    WsConnectionState* free_list_[SLAB_SIZE];
    size_t free_count_ = 0;

public:
    WsConnectionSlab() {
        for (size_t i = 0; i < SLAB_SIZE; i++) {
            free_list_[i] = &slab_[i];
        }
        free_count_ = SLAB_SIZE;
    }

    WsConnectionState* acquire() {
        if (free_count_ > 0) {
            auto* state = free_list_[--free_count_];
            state->reset();  // ensure clean state
            return state;
        }
        // Pool exhausted — fallback to heap
        return new WsConnectionState();
    }

    void release(WsConnectionState* state) {
        // Check if state belongs to our slab
        if (state >= &slab_[0] && state < &slab_[SLAB_SIZE]) {
            state->reset();
            free_list_[free_count_++] = state;
        } else {
            // Heap-allocated fallback
            delete state;
        }
    }
};

static WsConnectionSlab s_ws_slab;

} // anonymous namespace

// ── Python C API Integration ─────────────────────────────────────────────

static const char* WS_CAPSULE_NAME = "ws_connection_state";

namespace {

// Destructor for PyCapsule — returns state to slab
void ws_connection_state_destructor(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, WS_CAPSULE_NAME);
    if (ptr) {
        s_ws_slab.release(static_cast<WsConnectionState*>(ptr));
    }
}

// Helper to extract WsConnectionState from capsule
WsConnectionState* get_conn_state(PyObject* capsule) {
    void* ptr = PyCapsule_GetPointer(capsule, WS_CAPSULE_NAME);
    return static_cast<WsConnectionState*>(ptr);
}

} // anonymous namespace

PyObject* py_ws_ring_buffer_create(PyObject* /*self*/, PyObject* /*args*/) {
    WsConnectionState* state = s_ws_slab.acquire();
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
    state->ring.maybe_shrink();
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

    // Step 3: Parse + unmask + build echo
    // Pre-allocate PyBytes at max possible size, write directly into it.
    // This eliminates the thread_local vector → PyBytes copy.
    // Max echo output = data_len + 10 bytes header per frame (worst case: all 1-byte frames)
    size_t max_echo_size = data_len + 64;
    PyObject* echo_bytes = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)max_echo_size);
    if (!echo_bytes) return nullptr;
    uint8_t* echo_buf = (uint8_t*)PyBytes_AS_STRING(echo_bytes);

    EchoResult eres;
    size_t actual_echo_size = 0;

    // Always release GIL — syscall/memcpy work benefits from freeing other coroutines
    Py_BEGIN_ALLOW_THREADS
    actual_echo_size = ws_echo_frames_into_buffer(
        echo_buf, max_echo_size, data, data_len, eres);
    Py_END_ALLOW_THREADS

    // Shrink PyBytes in-place (no realloc when shrinking)
    if (actual_echo_size == 0) {
        Py_DECREF(echo_bytes);
        Py_INCREF(Py_None);
        echo_bytes = Py_None;
    } else if ((Py_ssize_t)actual_echo_size != PyBytes_GET_SIZE(echo_bytes)) {
        _PyBytes_Resize(&echo_bytes, (Py_ssize_t)actual_echo_size);
        if (!echo_bytes) return nullptr;
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

// ── ws_echo_direct_fd: Ultra-fast echo — writes directly to socket FD ────────
// Bypasses asyncio transport entirely. Uses scatter-gather writev() to echo
// frames with ZERO payload copy — payload stays in ring buffer, iovecs point at it.
// Returns: None (no data) | close_payload bytes (if CLOSE received)
// On the happy path, ZERO Python objects are allocated.

PyObject* py_ws_echo_direct_fd(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;
    int fd;

    if (!PyArg_ParseTuple(args, "Oy*i", &capsule, &py_buf, &fd)) {
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

    // Step 3: Scatter-gather echo via writev() — ZERO payload copy
    // Parse frames, unmask in-place, build frame headers into scratch buffer,
    // then writev() [header, payload_in_ring] pairs in a single syscall.
    static constexpr int MAX_WRITEV_FRAMES = 256;
    // Header scratch: max 10 bytes per frame header
    static thread_local uint8_t tl_hdr_buf[MAX_WRITEV_FRAMES * 10];
    // IOVec array: 2 entries per frame (header + payload)
    static thread_local PlatformIoVec tl_iov[MAX_WRITEV_FRAMES * 2];

    EchoResult eres;
    bool write_ok = true;
    size_t total_consumed = 0;
    int iov_count = 0;
    size_t hdr_offset = 0;
    // Hoisted from lambda so writev() failure fallback can access remaining iovecs
    int remaining_iovs = 0;
    PlatformIoVec* iov_ptr = tl_iov;

    auto do_echo = [&]() {
        while (total_consumed < data_len && iov_count < MAX_WRITEV_FRAMES * 2 - 1) {
            WsFrame frame;
            int consumed = ws_parse_frame(data + total_consumed,
                                          data_len - total_consumed, &frame);
            if (consumed <= 0) break;

            uint8_t opcode = (uint8_t)frame.opcode;

            if (opcode == WS_CLOSE) {
                eres.has_close = true;
                if (frame.masked && frame.payload_len > 0) {
                    ws_unmask((uint8_t*)frame.payload,
                              (size_t)frame.payload_len, frame.mask_key);
                }
                eres.close_payload_offset = (size_t)(frame.payload - data);
                eres.close_payload_len = (size_t)frame.payload_len;
                total_consumed += (size_t)consumed;
                break;
            }

            if (opcode == WS_PONG) {
                total_consumed += (size_t)consumed;
                continue;
            }

            // Unmask payload in-place (SIMD-accelerated for large payloads)
            if (frame.masked && frame.payload_len > 0) {
                ws_unmask((uint8_t*)frame.payload,
                          (size_t)frame.payload_len, frame.mask_key);
            }

            // Response opcode: PING → PONG, else echo same opcode
            WsOpcode resp_opcode = (opcode == WS_PING)
                ? WS_PONG : (WsOpcode)opcode;

            // Write frame header into scratch buffer
            size_t hdr_len = ws_write_frame_header(
                tl_hdr_buf + hdr_offset, resp_opcode,
                (size_t)frame.payload_len);

            // Header iovec
            tl_iov[iov_count].base = tl_hdr_buf + hdr_offset;
            tl_iov[iov_count].len = hdr_len;
            iov_count++;
            hdr_offset += hdr_len;

            // Payload iovec — zero-copy: points directly into ring buffer
            if (frame.payload_len > 0) {
                tl_iov[iov_count].base = (void*)frame.payload;
                tl_iov[iov_count].len = (size_t)frame.payload_len;
                iov_count++;
            }

            total_consumed += (size_t)consumed;
        }
        eres.total_consumed = total_consumed;

        // Single scatter-gather write — all frame headers + payloads in one syscall
        if (iov_count > 0 && fd >= 0) {
            remaining_iovs = iov_count;
            iov_ptr = tl_iov;
            while (remaining_iovs > 0) {
                ssize_t n = platform_socket_writev(fd, iov_ptr, remaining_iovs);
                if (n <= 0) { write_ok = false; break; }
                // Advance past fully-written iovecs (handle partial writes)
                size_t written = (size_t)n;
                while (remaining_iovs > 0 && written > 0) {
                    if (written >= iov_ptr->len) {
                        written -= iov_ptr->len;
                        iov_ptr++;
                        remaining_iovs--;
                    } else {
                        iov_ptr->base = (uint8_t*)iov_ptr->base + written;
                        iov_ptr->len -= written;
                        written = 0;
                    }
                }
            }
        }
    };

    // Always release GIL — writev() syscall (5-8µs) is the dominant cost,
    // and releasing GIL allows other Python coroutines to run during that time.
    Py_BEGIN_ALLOW_THREADS
    do_echo();
    Py_END_ALLOW_THREADS

    // Helper: collect remaining unsent iovecs into a PyBytes for transport.write() fallback
    auto build_fallback_bytes = [&]() -> PyObject* {
        if (!write_ok && remaining_iovs > 0) {
            size_t fallback_size = 0;
            for (int i = 0; i < remaining_iovs; i++)
                fallback_size += iov_ptr[i].len;
            if (fallback_size == 0) return nullptr;
            PyObject* fb = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)fallback_size);
            if (!fb) return nullptr;
            char* dst = PyBytes_AS_STRING(fb);
            for (int i = 0; i < remaining_iovs; i++) {
                memcpy(dst, iov_ptr[i].base, iov_ptr[i].len);
                dst += iov_ptr[i].len;
            }
            return fb;
        }
        return nullptr;
    };

    // Step 4: Extract close payload BEFORE consuming (data pointer validity)
    if (eres.has_close) {
        PyObject* close_result = PyBytes_FromStringAndSize(
            (const char*)(data + eres.close_payload_offset),
            (Py_ssize_t)eres.close_payload_len);
        if (eres.total_consumed > 0) {
            state->ring.consume(eres.total_consumed);
        }
        // writev() failed — return (fallback_bytes, close_payload) tuple
        PyObject* fallback = build_fallback_bytes();
        if (fallback) {
            PyObject* tup = PyTuple_Pack(2, fallback, close_result ? close_result : Py_None);
            Py_DECREF(fallback);
            Py_XDECREF(close_result);
            return tup;
        }
        return close_result;
    }

    // Step 5: Consume processed bytes
    if (eres.total_consumed > 0) {
        state->ring.consume(eres.total_consumed);
    }

    // writev() failed — return (fallback_bytes, None) tuple for Python transport.write()
    PyObject* fallback = build_fallback_bytes();
    if (fallback) {
        PyObject* tup = PyTuple_Pack(2, fallback, Py_None);
        Py_DECREF(fallback);
        return tup;
    }

    Py_RETURN_NONE;  // Happy path: zero allocations, zero copies!
}

// ── ws_echo_direct_fd_v2: Exclusive direct socket echo with EAGAIN buffering ──
// Like ws_echo_direct_fd but NEVER falls back to transport.write().
// On EAGAIN/partial write: buffers unsent data in WsConnectionState::output_pending
// for later retry via py_ws_flush_pending().
// Returns: 0 (success) | -1 (EAGAIN, data buffered) | (2, close_payload) for close

PyObject* py_ws_echo_direct_fd_v2(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;
    int fd;

    if (!PyArg_ParseTuple(args, "Oy*i", &capsule, &py_buf, &fd)) {
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
        return PyLong_FromLong(0);  // Ring buffer full — treat as success (backpressure)
    }

    // Step 2: Get contiguous readable data
    auto [data, data_len] = state->ring.readable_contiguous();
    if (data == nullptr || data_len == 0) {
        return PyLong_FromLong(0);  // No complete frames yet
    }

    // Step 3: Parse frames, build scatter-gather echo — same as v1
    static constexpr int MAX_WRITEV_FRAMES = 256;
    static thread_local uint8_t tl_hdr_buf[MAX_WRITEV_FRAMES * 10];
    static thread_local PlatformIoVec tl_iov[MAX_WRITEV_FRAMES * 2];

    EchoResult eres;
    bool write_ok = true;
    bool write_eagain = false;
    size_t total_consumed = 0;
    int iov_count = 0;
    size_t hdr_offset = 0;
    int remaining_iovs = 0;
    PlatformIoVec* iov_ptr = tl_iov;

    auto do_echo = [&]() {
        // First: flush any pending output from previous EAGAIN
        if (state->output_pending_len > 0) {
            ssize_t n = platform_socket_write(fd,
                state->output_pending, state->output_pending_len);
            if (n <= 0) {
                write_ok = false;
                write_eagain = true;
                return;  // Still can't write — buffer new data too
            }
            if ((size_t)n < state->output_pending_len) {
                // Partial write of pending — shift remaining
                size_t rem = state->output_pending_len - (size_t)n;
                memmove(state->output_pending,
                        state->output_pending + n, rem);
                state->output_pending_len = rem;
                write_ok = false;
                write_eagain = true;
                return;  // Can't write more yet
            }
            state->output_pending_len = 0;  // Fully flushed
        }

        // Parse frames and build iovecs
        while (total_consumed < data_len && iov_count < MAX_WRITEV_FRAMES * 2 - 1) {
            WsFrame frame;
            int consumed = ws_parse_frame(data + total_consumed,
                                          data_len - total_consumed, &frame);
            if (consumed <= 0) break;

            uint8_t opcode = (uint8_t)frame.opcode;

            if (opcode == WS_CLOSE) {
                eres.has_close = true;
                if (frame.masked && frame.payload_len > 0) {
                    ws_unmask((uint8_t*)frame.payload,
                              (size_t)frame.payload_len, frame.mask_key);
                }
                eres.close_payload_offset = (size_t)(frame.payload - data);
                eres.close_payload_len = (size_t)frame.payload_len;
                total_consumed += (size_t)consumed;
                break;
            }

            if (opcode == WS_PONG) {
                total_consumed += (size_t)consumed;
                continue;
            }

            // Unmask payload in-place
            if (frame.masked && frame.payload_len > 0) {
                ws_unmask((uint8_t*)frame.payload,
                          (size_t)frame.payload_len, frame.mask_key);
            }

            // Response opcode: PING → PONG, else echo same
            WsOpcode resp_opcode = (opcode == WS_PING)
                ? WS_PONG : (WsOpcode)opcode;

            // Write frame header into scratch buffer
            size_t hdr_len = ws_write_frame_header(
                tl_hdr_buf + hdr_offset, resp_opcode,
                (size_t)frame.payload_len);

            // Header iovec
            tl_iov[iov_count].base = tl_hdr_buf + hdr_offset;
            tl_iov[iov_count].len = hdr_len;
            iov_count++;
            hdr_offset += hdr_len;

            // Payload iovec — zero-copy from ring buffer
            if (frame.payload_len > 0) {
                tl_iov[iov_count].base = (void*)frame.payload;
                tl_iov[iov_count].len = (size_t)frame.payload_len;
                iov_count++;
            }

            total_consumed += (size_t)consumed;
        }
        eres.total_consumed = total_consumed;

        // Single scatter-gather write
        if (iov_count > 0 && fd >= 0) {
            remaining_iovs = iov_count;
            iov_ptr = tl_iov;
            while (remaining_iovs > 0) {
                ssize_t n = platform_socket_writev(fd, iov_ptr, remaining_iovs);
                if (n <= 0) {
                    write_ok = false;
                    write_eagain = true;
                    break;
                }
                // Advance past fully-written iovecs
                size_t written = (size_t)n;
                while (remaining_iovs > 0 && written > 0) {
                    if (written >= iov_ptr->len) {
                        written -= iov_ptr->len;
                        iov_ptr++;
                        remaining_iovs--;
                    } else {
                        iov_ptr->base = (uint8_t*)iov_ptr->base + written;
                        iov_ptr->len -= written;
                        written = 0;
                    }
                }
            }
        }
    };

    // Release GIL for writev() syscall
    Py_BEGIN_ALLOW_THREADS
    do_echo();
    Py_END_ALLOW_THREADS

    // Buffer any unsent iovecs into output_pending (instead of transport.write fallback)
    if (!write_ok && remaining_iovs > 0) {
        for (int i = 0; i < remaining_iovs; i++) {
            state->buffer_output(iov_ptr[i].base, iov_ptr[i].len);
        }
    }

    // Handle close frame
    if (eres.has_close) {
        PyObject* close_bytes = PyBytes_FromStringAndSize(
            (const char*)(data + eres.close_payload_offset),
            (Py_ssize_t)eres.close_payload_len);
        if (eres.total_consumed > 0) {
            state->ring.consume(eres.total_consumed);
        }
        // Return (2, close_payload)
        PyObject* two = PyLong_FromLong(2);
        if (!two) { Py_XDECREF(close_bytes); return nullptr; }
        PyObject* tup = PyTuple_Pack(2, two, close_bytes ? close_bytes : Py_None);
        Py_DECREF(two);
        Py_XDECREF(close_bytes);
        return tup;
    }

    // Consume processed bytes
    if (eres.total_consumed > 0) {
        state->ring.consume(eres.total_consumed);
    }

    // Return 0 (success) or -1 (EAGAIN, data buffered for later flush)
    return PyLong_FromLong(write_eagain ? -1 : 0);
}

// ── ws_flush_pending: Retry sending buffered output via direct socket ────────
// Returns: 0 (all flushed) | -1 (still pending, retry later)

PyObject* py_ws_flush_pending(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    int fd;

    if (!PyArg_ParseTuple(args, "Oi", &capsule, &fd)) {
        return nullptr;
    }

    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }

    if (state->output_pending_len == 0) {
        return PyLong_FromLong(0);  // Nothing pending
    }

    ssize_t n;
    Py_BEGIN_ALLOW_THREADS
    n = platform_socket_write(fd,
        state->output_pending, state->output_pending_len);
    Py_END_ALLOW_THREADS

    if (n <= 0) {
        return PyLong_FromLong(-1);  // Still can't write
    }

    if ((size_t)n >= state->output_pending_len) {
        state->output_pending_len = 0;  // Fully flushed
        return PyLong_FromLong(0);
    }

    // Partial write — shift remaining data
    size_t rem = state->output_pending_len - (size_t)n;
    memmove(state->output_pending,
            state->output_pending + n, rem);
    state->output_pending_len = rem;
    return PyLong_FromLong(-1);  // Still pending
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
        PyObject* tuple = make_frame_tuple(WS_CLOSE, payload_obj);
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

    // Step 3b: Update C++ metrics (avoids per-frame Python attribute stores)
    for (size_t i = 0; i < pres.frames.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.frames[i].payload_len;
    }
    for (size_t i = 0; i < pres.assembled.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.assembled[i].payload.size();
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

        PyObject* tuple = make_frame_tuple(fr.opcode, payload_obj);
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

        PyObject* tuple = make_frame_tuple(msg.opcode, payload_obj);
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

// ── ws_handle_and_feed: Parse frames + feed directly to channel ─────────────
// Eliminates the intermediate Python list of tuples — feeds frames directly
// into the channel's waiter/buffer from C++.
//
// Args: (capsule, data, waiter_or_none, buffer_deque)
// Returns: (pong_bytes|None, close_payload|None, frames_fed_count)
// The waiter (asyncio.Future) is resolved directly from C++ if available.
// Remaining frames are appended to buffer_deque.

PyObject* py_ws_handle_and_feed(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_buffer py_buf;
    PyObject* waiter;     // asyncio.Future or None
    PyObject* buffer_obj; // collections.deque

    if (!PyArg_ParseTuple(args, "Oy*OO", &capsule, &py_buf, &waiter, &buffer_obj)) {
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
        Py_RETURN_NONE;
    }

    // Step 2: Get contiguous readable data
    auto [data, data_len] = state->ring.readable_contiguous();
    if (data == nullptr || data_len == 0) {
        Py_RETURN_NONE;
    }

    // Step 3: Parse + unmask all frames (GIL released)
    ParseResult pres;
    Py_BEGIN_ALLOW_THREADS
    ws_parse_frames_nogil(data, data_len, pres, &state->assembler);
    Py_END_ALLOW_THREADS

    if (pres.protocol_error != 0) {
        // Protocol error — return close indicator
        uint8_t close_payload[2] = { 0x03, 0xEA };
        PyObject* close_bytes = PyBytes_FromStringAndSize((const char*)close_payload, 2);
        if (!close_bytes) return nullptr;
        return Py_BuildValue("(OOi)", Py_None, close_bytes, 0);
    }

    if (pres.total_consumed == 0 && pres.frames.empty() && pres.assembled.empty()) {
        Py_RETURN_NONE;
    }

    // Step 3b: Update C++ metrics
    for (size_t i = 0; i < pres.frames.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.frames[i].payload_len;
    }
    for (size_t i = 0; i < pres.assembled.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.assembled[i].payload.size();
    }

    // Step 4: Feed frames directly to channel (waiter/buffer)
    // Check waiter: must not be None and not done
    bool waiter_available = (waiter != Py_None);
    if (waiter_available) {
        // Check if waiter.done() is True — if so, can't resolve it
        PyRef done_result(PyObject_CallMethodNoArgs(waiter, g_str_done));
        if (done_result && PyObject_IsTrue(done_result.get())) {
            waiter_available = false;
        }
    }

    PyObject* close_payload_obj = nullptr;
    int frames_fed = 0;

    // Helper lambda to build payload PyObject
    auto build_payload = [&](uint8_t opcode, const char* payload_ptr, size_t payload_len) -> PyObject* {
        if (opcode == WS_TEXT) {
            PyObject* obj = PyUnicode_DecodeUTF8(payload_ptr, (Py_ssize_t)payload_len, "surrogateescape");
            if (!obj) {
                PyErr_Clear();
                obj = PyBytes_FromStringAndSize(payload_ptr, (Py_ssize_t)payload_len);
            }
            return obj;
        }
        return PyBytes_FromStringAndSize(payload_ptr, (Py_ssize_t)payload_len);
    };

    // Feed inline frames
    for (size_t i = 0; i < pres.frames.size(); i++) {
        const auto& fr = pres.frames[i];
        const char* payload_ptr = (const char*)(data + fr.payload_offset);

        if (fr.opcode == WS_CLOSE) {
            close_payload_obj = PyBytes_FromStringAndSize(
                payload_ptr, (Py_ssize_t)fr.payload_len);
            if (!close_payload_obj) return nullptr;
            // Feed close to waiter or buffer
            PyRef tuple(PyTuple_New(2));
            if (!tuple) { Py_DECREF(close_payload_obj); return nullptr; }
            Py_INCREF(g_opcode_close);
            PyTuple_SET_ITEM(tuple.get(), 0, g_opcode_close);
            Py_INCREF(close_payload_obj);
            PyTuple_SET_ITEM(tuple.get(), 1, close_payload_obj);
            if (waiter_available) {
                PyRef sr(PyObject_CallMethod(waiter, "set_result", "(O)", tuple.get()));
                waiter_available = false;
            } else {
                PyRef ar(PyObject_CallMethod(buffer_obj, "append", "(O)", tuple.get()));
            }
            frames_fed++;
            break;
        }

        PyObject* payload_obj = build_payload(fr.opcode, payload_ptr, fr.payload_len);
        if (!payload_obj) return nullptr;

        PyObject* tuple = make_frame_tuple(fr.opcode, payload_obj);
        if (!tuple) return nullptr;

        if (waiter_available) {
            PyRef sr(PyObject_CallMethod(waiter, "set_result", "(O)", tuple));
            Py_DECREF(tuple);
            waiter_available = false;
        } else {
            PyRef ar(PyObject_CallMethod(buffer_obj, "append", "(O)", tuple));
            Py_DECREF(tuple);
        }
        frames_fed++;
    }

    // Feed assembled (multi-fragment) messages
    if (!close_payload_obj) {
        for (size_t i = 0; i < pres.assembled.size(); i++) {
            const auto& msg = pres.assembled[i];
            const char* payload_ptr = (const char*)msg.payload.data();

            PyObject* payload_obj = build_payload(msg.opcode, payload_ptr, msg.payload.size());
            if (!payload_obj) return nullptr;

            PyObject* tuple = make_frame_tuple(msg.opcode, payload_obj);
            if (!tuple) return nullptr;

            if (waiter_available) {
                PyRef sr(PyObject_CallMethod(waiter, "set_result", "(O)", tuple));
                Py_DECREF(tuple);
                waiter_available = false;
            } else {
                PyRef ar(PyObject_CallMethod(buffer_obj, "append", "(O)", tuple));
                Py_DECREF(tuple);
            }
            frames_fed++;
        }
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

    // Return (pong_bytes, close_payload, frames_count)
    PyObject* close_ret = close_payload_obj ? close_payload_obj : Py_None;
    if (!close_payload_obj) Py_INCREF(Py_None);
    PyObject* result = Py_BuildValue("(OOi)", py_pong, close_ret, frames_fed);
    Py_DECREF(py_pong);
    if (close_payload_obj) Py_DECREF(close_payload_obj);
    else Py_DECREF(Py_None);
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
        PyObject* tuple = make_frame_tuple(WS_CLOSE, payload_obj);
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

    // Step 3b: Update C++ metrics
    for (size_t i = 0; i < pres.frames.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.frames[i].payload_len;
    }
    for (size_t i = 0; i < pres.assembled.size(); i++) {
        state->messages_in++;
        state->bytes_in += pres.assembled[i].payload.size();
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

        PyObject* tuple = make_frame_tuple(fr.opcode, payload_obj);
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

        PyObject* tuple = make_frame_tuple(msg.opcode, payload_obj);
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

    PyObject* result_json = PyTuple_Pack(2, frames.get(), py_pong);
    Py_DECREF(py_pong);
    return result_json;
}

// ── ws_get_metrics: Return C++ metrics as tuple ─────────────────────────────
// Returns (messages_in, bytes_in, messages_out, bytes_out) from C++ counters.

PyObject* py_ws_get_metrics(PyObject* /*self*/, PyObject* capsule) {
    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }
    return Py_BuildValue("(KKKK)",
        (unsigned long long)state->messages_in,
        (unsigned long long)state->bytes_in,
        (unsigned long long)state->messages_out,
        (unsigned long long)state->bytes_out);
}

// ── ws_update_send_metrics: Update send counters from Python side ───────────
// Called after send_text/send_bytes to update C++ metrics.
// Args: (capsule, bytes_count)

PyObject* py_ws_update_send_metrics(PyObject* /*self*/, PyObject* args) {
    PyObject* capsule;
    Py_ssize_t byte_count;
    if (!PyArg_ParseTuple(args, "On", &capsule, &byte_count)) {
        return nullptr;
    }
    WsConnectionState* state = get_conn_state(capsule);
    if (!state) {
        PyErr_SetString(PyExc_ValueError, "Invalid ws_connection_state capsule");
        return nullptr;
    }
    state->messages_out++;
    state->bytes_out += (uint64_t)byte_count;
    Py_RETURN_NONE;
}
