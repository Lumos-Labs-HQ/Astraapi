#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "ws_frame_parser.hpp"
#include "pyref.hpp"
#include <cstring>
#include <string>

// ── SIMD headers ─────────────────────────────────────────────────────────────
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

// ── Minimal SHA-1 (RFC 3174) — only for WebSocket handshake ─────────────────
// Security note: SHA-1 is used here ONLY for RFC 6455 protocol compliance,
// NOT for cryptographic purposes.

namespace {

struct SHA1 {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];

    SHA1() {
        state[0] = 0x67452301;
        state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE;
        state[3] = 0x10325476;
        state[4] = 0xC3D2E1F0;
        count = 0;
        memset(buffer, 0, 64);
    }

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void transform(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
                    ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                 k = 0xCA62C1D6; }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        size_t idx = (size_t)(count % 64);
        count += len;

        for (size_t i = 0; i < len; i++) {
            buffer[idx++] = data[i];
            if (idx == 64) {
                transform(buffer);
                idx = 0;
            }
        }
    }

    void finalize(uint8_t digest[20]) {
        uint64_t bits = count * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while ((count % 64) != 56) update(&pad, 1);

        uint8_t len_be[8];
        for (int i = 7; i >= 0; i--) {
            len_be[i] = (uint8_t)(bits & 0xFF);
            bits >>= 8;
        }
        update(len_be, 8);

        for (int i = 0; i < 5; i++) {
            digest[i*4]     = (uint8_t)(state[i] >> 24);
            digest[i*4 + 1] = (uint8_t)(state[i] >> 16);
            digest[i*4 + 2] = (uint8_t)(state[i] >> 8);
            digest[i*4 + 3] = (uint8_t)(state[i]);
        }
    }
};

// ── Base64 encode ───────────────────────────────────────────────────────────

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i+1]) << 8;
        if (i + 2 < len) n |= (uint32_t)data[i+2];

        result.push_back(b64_table[(n >> 18) & 0x3F]);
        result.push_back(b64_table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? b64_table[n & 0x3F] : '=');
    }
    return result;
}

} // anonymous namespace

// ── Frame parser ────────────────────────────────────────────────────────────

int ws_parse_frame(const uint8_t* data, size_t len, WsFrame* out) {
    if (len < 2) return 0;  // need more data

    out->fin = (data[0] & 0x80) != 0;
    out->opcode = (WsOpcode)(data[0] & 0x0F);
    out->masked = (data[1] & 0x80) != 0;

    uint64_t payload_len = data[1] & 0x7F;
    size_t pos = 2;

    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = ((uint64_t)data[2] << 8) | (uint64_t)data[3];
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | (uint64_t)data[2 + i];
        }
        pos = 10;
    }

    // Limit payload to 64MB to prevent abuse
    if (payload_len > 64 * 1024 * 1024) return -1;

    if (out->masked) {
        if (len < pos + 4) return 0;
        memcpy(out->mask_key, data + pos, 4);
        pos += 4;
    } else {
        memset(out->mask_key, 0, 4);
    }

    if (len < pos + payload_len) return 0;  // need more data

    out->payload_len = payload_len;
    out->payload = data + pos;
    out->header_len = pos;

    return (int)(pos + payload_len);
}

// ── SIMD-optimized unmasking ────────────────────────────────────────────────

void ws_unmask(uint8_t* payload, size_t len, const uint8_t mask[4]) {
    size_t i = 0;

#if defined(__SSE2__)
    // SSE2: unmask 16 bytes at a time
    if (len >= 16) {
        // Replicate 4-byte mask to fill 16 bytes
        uint8_t mask16[16];
        for (int j = 0; j < 16; j++) mask16[j] = mask[j & 3];
        __m128i mask_vec = _mm_loadu_si128((__m128i*)mask16);

        for (; i + 15 < len; i += 16) {
            __m128i data_vec = _mm_loadu_si128((__m128i*)(payload + i));
            __m128i result = _mm_xor_si128(data_vec, mask_vec);
            _mm_storeu_si128((__m128i*)(payload + i), result);
        }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // NEON: unmask 16 bytes at a time
    if (len >= 16) {
        uint8_t mask16[16];
        for (int j = 0; j < 16; j++) mask16[j] = mask[j & 3];
        uint8x16_t mask_vec = vld1q_u8(mask16);

        for (; i + 15 < len; i += 16) {
            uint8x16_t data_vec = vld1q_u8(payload + i);
            uint8x16_t result = veorq_u8(data_vec, mask_vec);
            vst1q_u8(payload + i, result);
        }
    }
#endif

    // 8-byte scalar path for remaining bytes (or entire buffer if no SIMD)
    if (i < len && len - i >= 8) {
        uint32_t mask32;
        memcpy(&mask32, mask, 4);
        uint64_t mask64 = ((uint64_t)mask32 << 32) | (uint64_t)mask32;
        for (; i + 7 < len; i += 8) {
            uint64_t chunk;
            memcpy(&chunk, payload + i, 8);
            chunk ^= mask64;
            memcpy(payload + i, &chunk, 8);
        }
    }

    // Byte-by-byte tail
    for (; i < len; i++) {
        payload[i] ^= mask[i & 3];
    }
}

// ── Frame header writer ────────────────────────────────────────────────────

size_t ws_write_frame_header(uint8_t* buf, WsOpcode opcode, size_t payload_len, bool fin) {
    buf[0] = (fin ? 0x80 : 0x00) | (uint8_t)opcode;

    if (payload_len < 126) {
        buf[1] = (uint8_t)payload_len;
        return 2;
    } else if (payload_len <= 0xFFFF) {
        buf[1] = 126;
        buf[2] = (uint8_t)(payload_len >> 8);
        buf[3] = (uint8_t)(payload_len & 0xFF);
        return 4;
    } else {
        buf[1] = 127;
        for (int i = 7; i >= 0; i--) {
            buf[2 + (7 - i)] = (uint8_t)((payload_len >> (i * 8)) & 0xFF);
        }
        return 10;
    }
}

// ── Frame builder (legacy — returns vector) ─────────────────────────────────

std::vector<uint8_t> ws_build_frame(WsOpcode opcode, const uint8_t* payload, size_t len, bool fin) {
    size_t hdr_size = ws_frame_header_size(len);
    std::vector<uint8_t> frame(hdr_size + len);
    ws_write_frame_header(frame.data(), opcode, len, fin);
    memcpy(frame.data() + hdr_size, payload, len);
    return frame;
}

std::vector<uint8_t> ws_build_close_frame(uint16_t status_code) {
    uint8_t payload[2] = { (uint8_t)(status_code >> 8), (uint8_t)(status_code & 0xFF) };
    return ws_build_frame(WS_CLOSE, payload, 2);
}

// ── WebSocket upgrade handshake (RFC 6455 §4.2.2) ──────────────────────────

static const char WS_MAGIC_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::vector<char> ws_build_upgrade_response(const char* sec_key, size_t key_len) {
    // Compute Sec-WebSocket-Accept = base64(SHA1(key + magic_guid))
    SHA1 sha;
    sha.update((const uint8_t*)sec_key, key_len);
    sha.update((const uint8_t*)WS_MAGIC_GUID, sizeof(WS_MAGIC_GUID) - 1);
    uint8_t digest[20];
    sha.finalize(digest);
    std::string accept = base64_encode(digest, 20);

    // Build HTTP 101 response
    std::string resp;
    resp.reserve(256);
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n\r\n";

    return std::vector<char>(resp.begin(), resp.end());
}

// ══════════════════════════════════════════════════════════════════════════════
// Python-callable high-performance WebSocket functions
// ══════════════════════════════════════════════════════════════════════════════

// ── ws_parse_frames(buffer: bytearray) -> (consumed, [(opcode, payload), ...], pong_bytes|None)
// Parses all complete frames from buffer in one C call.
// Unmaskes payloads, auto-generates pong for ping frames.

PyObject* py_ws_parse_frames(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;
    size_t total_consumed = 0;

    // Pre-allocate list (most common: 1-4 frames per call)
    PyRef frames(PyList_New(0));
    if (!frames) { PyBuffer_Release(&buf); return nullptr; }

    // Accumulate pong responses (rare — usually 0)
    std::vector<uint8_t> pong_buf;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);

        if (consumed <= 0) break;  // need more data or error

        // Unmask in-place
        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        uint8_t opcode = (uint8_t)frame.opcode;

        if (opcode == WS_PING) {
            // Auto-build pong response, append to pong_buf
            size_t pong_hdr = ws_frame_header_size((size_t)frame.payload_len);
            size_t old_size = pong_buf.size();
            pong_buf.resize(old_size + pong_hdr + (size_t)frame.payload_len);
            ws_write_frame_header(pong_buf.data() + old_size, WS_PONG, (size_t)frame.payload_len);
            if (frame.payload_len > 0) {
                memcpy(pong_buf.data() + old_size + pong_hdr, frame.payload, (size_t)frame.payload_len);
            }
            total_consumed += (size_t)consumed;
            continue;
        }

        if (opcode == WS_PONG) {
            // Ignore pong frames
            total_consumed += (size_t)consumed;
            continue;
        }

        // Data frame or close — create (opcode, payload) tuple
        PyRef payload_bytes(PyBytes_FromStringAndSize((const char*)frame.payload, (Py_ssize_t)frame.payload_len));
        if (!payload_bytes) { PyBuffer_Release(&buf); return nullptr; }

        PyRef tuple(PyTuple_Pack(2, PyLong_FromLong(opcode), payload_bytes.get()));
        if (!tuple) { PyBuffer_Release(&buf); return nullptr; }
        // Fix refcount: PyTuple_Pack increfs, but PyLong_FromLong returns new ref
        // We need to decref the opcode long since PyTuple_Pack stole our ref
        Py_DECREF(PyTuple_GET_ITEM(tuple.get(), 0));  // balance the PyLong_FromLong

        if (PyList_Append(frames.get(), tuple.get()) < 0) {
            PyBuffer_Release(&buf);
            return nullptr;
        }

        total_consumed += (size_t)consumed;

        if (opcode == WS_CLOSE) {
            break;  // stop processing after close
        }
    }

    PyBuffer_Release(&buf);

    // Build result: (consumed, frames_list, pong_bytes_or_none)
    PyRef py_consumed(PyLong_FromSize_t(total_consumed));

    PyObject* py_pong;
    if (!pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize((const char*)pong_buf.data(), (Py_ssize_t)pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    return PyTuple_Pack(3, py_consumed.get(), frames.get(), py_pong);
}

// ── ws_build_frame_bytes(opcode: int, payload: bytes|str) -> bytes
// Builds a single server→client frame. Writes directly into PyBytes buffer.

PyObject* py_ws_build_frame_bytes(PyObject* /*self*/, PyObject* args) {
    int opcode;
    Py_buffer payload_buf;

    if (!PyArg_ParseTuple(args, "iy*", &opcode, &payload_buf)) {
        return nullptr;
    }

    size_t payload_len = (size_t)payload_buf.len;
    size_t hdr_size = ws_frame_header_size(payload_len);
    size_t total = hdr_size + payload_len;

    // Allocate PyBytes directly — zero copy
    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
    if (!result) {
        PyBuffer_Release(&payload_buf);
        return nullptr;
    }

    uint8_t* out = (uint8_t*)PyBytes_AS_STRING(result);
    ws_write_frame_header(out, (WsOpcode)opcode, payload_len);
    memcpy(out + hdr_size, payload_buf.buf, payload_len);

    PyBuffer_Release(&payload_buf);
    return result;
}

// ── ws_build_close_frame_bytes(code: int) -> bytes

PyObject* py_ws_build_close_frame_bytes(PyObject* /*self*/, PyObject* arg) {
    long code = PyLong_AsLong(arg);
    if (code == -1 && PyErr_Occurred()) return nullptr;

    uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
    size_t hdr_size = ws_frame_header_size(2);
    size_t total = hdr_size + 2;

    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
    if (!result) return nullptr;

    uint8_t* out = (uint8_t*)PyBytes_AS_STRING(result);
    ws_write_frame_header(out, WS_CLOSE, 2);
    memcpy(out + hdr_size, payload, 2);
    return result;
}

// ── ws_parse_frames_text(buffer: bytearray) -> (consumed, [(opcode, str|bytes), ...], pong_bytes|None)
// Like ws_parse_frames but decodes TEXT frame payloads as UTF-8 str in C++,
// avoiding a separate .decode("utf-8") call in Python.

PyObject* py_ws_parse_frames_text(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;
    size_t total_consumed = 0;

    PyRef frames(PyList_New(0));
    if (!frames) { PyBuffer_Release(&buf); return nullptr; }

    std::vector<uint8_t> pong_buf;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);
        if (consumed <= 0) break;

        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        uint8_t opcode = (uint8_t)frame.opcode;

        if (opcode == WS_PING) {
            size_t pong_hdr = ws_frame_header_size((size_t)frame.payload_len);
            size_t old_size = pong_buf.size();
            pong_buf.resize(old_size + pong_hdr + (size_t)frame.payload_len);
            ws_write_frame_header(pong_buf.data() + old_size, WS_PONG, (size_t)frame.payload_len);
            if (frame.payload_len > 0) {
                memcpy(pong_buf.data() + old_size + pong_hdr, frame.payload, (size_t)frame.payload_len);
            }
            total_consumed += (size_t)consumed;
            continue;
        }

        if (opcode == WS_PONG) {
            total_consumed += (size_t)consumed;
            continue;
        }

        // For TEXT frames, decode payload as UTF-8 str directly
        PyObject* payload_obj;
        if (opcode == WS_TEXT) {
            payload_obj = PyUnicode_DecodeUTF8(
                (const char*)frame.payload, (Py_ssize_t)frame.payload_len, "surrogateescape");
            if (!payload_obj) {
                // Fallback to bytes on decode error
                payload_obj = PyBytes_FromStringAndSize(
                    (const char*)frame.payload, (Py_ssize_t)frame.payload_len);
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(
                (const char*)frame.payload, (Py_ssize_t)frame.payload_len);
        }
        if (!payload_obj) { PyBuffer_Release(&buf); return nullptr; }

        PyRef payload_ref(payload_obj);
        PyRef tuple(PyTuple_Pack(2, PyLong_FromLong(opcode), payload_ref.get()));
        if (!tuple) { PyBuffer_Release(&buf); return nullptr; }
        Py_DECREF(PyTuple_GET_ITEM(tuple.get(), 0));  // balance PyLong_FromLong

        if (PyList_Append(frames.get(), tuple.get()) < 0) {
            PyBuffer_Release(&buf);
            return nullptr;
        }

        total_consumed += (size_t)consumed;
        if (opcode == WS_CLOSE) break;
    }

    PyBuffer_Release(&buf);

    PyRef py_consumed(PyLong_FromSize_t(total_consumed));
    PyObject* py_pong;
    if (!pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize((const char*)pong_buf.data(), (Py_ssize_t)pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    return PyTuple_Pack(3, py_consumed.get(), frames.get(), py_pong);
}

// ── ws_echo_frames(buffer: bytearray) -> (consumed, echo_bytes, close_payload|None)
// Parse all incoming frames, unmask, and build echo response frames in one C++ call.
// Returns all echo responses as a single contiguous bytes buffer.
// For PING: auto-generates PONG (included in echo_bytes).
// For CLOSE: stops and returns close payload separately.

PyObject* py_ws_echo_frames(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;
    size_t total_consumed = 0;

    // First pass: compute total echo output size
    size_t echo_size = 0;
    size_t saved_consumed = 0;
    bool has_close = false;
    size_t close_offset = 0;

    {
        size_t scan = 0;
        while (scan < data_len) {
            WsFrame frame;
            int consumed = ws_parse_frame(data + scan, data_len - scan, &frame);
            if (consumed <= 0) break;

            uint8_t opcode = (uint8_t)frame.opcode;
            if (opcode == WS_CLOSE) {
                has_close = true;
                close_offset = scan;
                saved_consumed = scan + (size_t)consumed;
                break;
            } else if (opcode == WS_PING) {
                // PONG response
                echo_size += ws_frame_header_size((size_t)frame.payload_len) + (size_t)frame.payload_len;
            } else if (opcode == WS_PONG) {
                // ignore
            } else {
                // Echo: same opcode, unmasked payload
                echo_size += ws_frame_header_size((size_t)frame.payload_len) + (size_t)frame.payload_len;
            }
            scan += (size_t)consumed;
        }
        if (!has_close) {
            saved_consumed = scan;
        }
    }

    total_consumed = saved_consumed;

    // Allocate echo output buffer
    PyObject* echo_bytes;
    if (echo_size > 0) {
        echo_bytes = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)echo_size);
        if (!echo_bytes) { PyBuffer_Release(&buf); return nullptr; }
    } else {
        Py_INCREF(Py_None);
        echo_bytes = Py_None;
    }

    // Second pass: unmask + write echo frames
    if (echo_size > 0) {
        uint8_t* out = (uint8_t*)PyBytes_AS_STRING(echo_bytes);
        size_t out_offset = 0;
        size_t scan = 0;

        while (scan < total_consumed) {
            WsFrame frame;
            int consumed = ws_parse_frame(data + scan, data_len - scan, &frame);
            if (consumed <= 0) break;

            // Unmask in-place
            if (frame.masked && frame.payload_len > 0) {
                ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
            }

            uint8_t opcode = (uint8_t)frame.opcode;
            if (opcode == WS_PING) {
                size_t hdr = ws_write_frame_header(out + out_offset, WS_PONG, (size_t)frame.payload_len);
                out_offset += hdr;
                if (frame.payload_len > 0) {
                    memcpy(out + out_offset, frame.payload, (size_t)frame.payload_len);
                    out_offset += (size_t)frame.payload_len;
                }
            } else if (opcode == WS_PONG) {
                // skip
            } else if (opcode == WS_CLOSE) {
                break;
            } else {
                // Echo frame
                size_t hdr = ws_write_frame_header(out + out_offset, (WsOpcode)opcode, (size_t)frame.payload_len);
                out_offset += hdr;
                if (frame.payload_len > 0) {
                    memcpy(out + out_offset, frame.payload, (size_t)frame.payload_len);
                    out_offset += (size_t)frame.payload_len;
                }
            }
            scan += (size_t)consumed;
        }
    } else if (has_close) {
        // Need to unmask close frame payload
        WsFrame frame;
        int consumed = ws_parse_frame(data + close_offset, data_len - close_offset, &frame);
        if (consumed > 0 && frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }
    }

    PyBuffer_Release(&buf);

    // Build close payload
    PyObject* close_obj;
    if (has_close) {
        // Re-parse close frame to get unmasked payload
        // (already unmasked in second pass above or else-if)
        WsFrame frame;
        ws_parse_frame(data + close_offset, data_len - close_offset, &frame);
        close_obj = PyBytes_FromStringAndSize(
            (const char*)frame.payload, (Py_ssize_t)frame.payload_len);
        if (!close_obj) { Py_DECREF(echo_bytes); return nullptr; }
    } else {
        Py_INCREF(Py_None);
        close_obj = Py_None;
    }

    PyRef py_consumed(PyLong_FromSize_t(total_consumed));
    PyObject* result = PyTuple_Pack(3, py_consumed.get(), echo_bytes, close_obj);
    Py_DECREF(echo_bytes);
    Py_DECREF(close_obj);
    return result;
}

// ── ws_build_frames_batch(messages: list[tuple[int, bytes]]) -> bytes
// Builds multiple frames into a single contiguous bytes buffer for write batching.

PyObject* py_ws_build_frames_batch(PyObject* /*self*/, PyObject* arg) {
    if (!PyList_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "expected list of (opcode, payload) tuples");
        return nullptr;
    }

    Py_ssize_t n = PyList_GET_SIZE(arg);

    // First pass: compute total size
    size_t total_size = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyList_GET_ITEM(arg, i);
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            PyErr_SetString(PyExc_TypeError, "each item must be (opcode, payload) tuple");
            return nullptr;
        }
        PyObject* payload_obj = PyTuple_GET_ITEM(item, 1);
        Py_ssize_t plen;
        if (PyBytes_Check(payload_obj)) {
            plen = PyBytes_GET_SIZE(payload_obj);
        } else if (PyUnicode_Check(payload_obj)) {
            PyUnicode_AsUTF8AndSize(payload_obj, &plen);
        } else {
            PyErr_SetString(PyExc_TypeError, "payload must be bytes or str");
            return nullptr;
        }
        total_size += ws_frame_header_size((size_t)plen) + (size_t)plen;
    }

    // Allocate single PyBytes for all frames
    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total_size);
    if (!result) return nullptr;

    uint8_t* out = (uint8_t*)PyBytes_AS_STRING(result);
    size_t offset = 0;

    // Second pass: write frames
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyList_GET_ITEM(arg, i);
        int opcode = (int)PyLong_AsLong(PyTuple_GET_ITEM(item, 0));
        PyObject* payload_obj = PyTuple_GET_ITEM(item, 1);

        const char* pdata;
        Py_ssize_t plen;
        if (PyBytes_Check(payload_obj)) {
            pdata = PyBytes_AS_STRING(payload_obj);
            plen = PyBytes_GET_SIZE(payload_obj);
        } else {
            pdata = PyUnicode_AsUTF8AndSize(payload_obj, &plen);
        }

        size_t hdr = ws_write_frame_header(out + offset, (WsOpcode)opcode, (size_t)plen);
        offset += hdr;
        memcpy(out + offset, pdata, (size_t)plen);
        offset += (size_t)plen;
    }

    return result;
}
