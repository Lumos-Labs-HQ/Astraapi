#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "ws_frame_parser.hpp"
#include "platform.hpp"    // fast_memcpy_small (OPT-18)
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>

// ── SIMD headers ─────────────────────────────────────────────────────────────
#if defined(__AVX2__)
#include <immintrin.h>   // AVX2 + SSE2
#elif defined(__SSE2__)
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

int ws_parse_frame(const uint8_t* data, size_t len, WsFrame* out,
                   bool require_mask, bool allow_rsv) {
    if (len < 2) return 0;  // need more data

    out->fin = (data[0] & 0x80) != 0;
    out->rsv = (data[0] >> 4) & 0x07;  // RSV1-3 bits
    out->opcode = (WsOpcode)(data[0] & 0x0F);
    out->masked = (data[1] & 0x80) != 0;

    // Validate RSV bits — must be 0 unless extensions are negotiated (RFC 6455 §5.2).
    // Strip silently rather than closing: some browsers/tools send RSV1=1 for
    // permessage-deflate even when the server didn't accept the extension.
    // A hard 1002 close is worse UX than accepting a slightly garbled frame.
    if (out->rsv != 0 && !allow_rsv) {
        out->rsv = 0;  // strip RSV bits, continue
    }

    // Validate opcode — reject reserved opcodes (RFC 6455 §5.2)
    uint8_t op = (uint8_t)out->opcode;
    if ((op >= 3 && op <= 7) || (op >= 0xB)) {
        return -3;
    }

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

    // Reject unmasked client frames (RFC 6455 §5.1)
    if (require_mask && !out->masked) {
        return -2;
    }

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

#if defined(__AVX2__)
    // AVX2: unmask 32 bytes at a time — broadcast mask via single instruction
    if (len >= 32) {
        uint32_t m;
        memcpy(&m, mask, 4);
        __m256i mask_vec = _mm256_set1_epi32((int)m);

        for (; i + 31 < len; i += 32) {
            __m256i d = _mm256_loadu_si256((__m256i*)(payload + i));
            _mm256_storeu_si256((__m256i*)(payload + i), _mm256_xor_si256(d, mask_vec));
        }
    }
    // SSE2 for 16-byte remainder (AVX2 implies SSE2)
    if (i + 15 < len) {
        uint32_t m;
        memcpy(&m, mask, 4);
        __m128i mask_vec = _mm_set1_epi32((int)m);

        for (; i + 15 < len; i += 16) {
            __m128i data_vec = _mm_loadu_si128((__m128i*)(payload + i));
            _mm_storeu_si128((__m128i*)(payload + i), _mm_xor_si128(data_vec, mask_vec));
        }
    }
#elif defined(__SSE2__)
    // SSE2: unmask 16 bytes at a time — broadcast mask via single instruction
    if (len >= 16) {
        uint32_t m;
        memcpy(&m, mask, 4);
        __m128i mask_vec = _mm_set1_epi32((int)m);

        for (; i + 15 < len; i += 16) {
            __m128i data_vec = _mm_loadu_si128((__m128i*)(payload + i));
            __m128i result = _mm_xor_si128(data_vec, mask_vec);
            _mm_storeu_si128((__m128i*)(payload + i), result);
        }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // NEON: unmask 16 bytes at a time — broadcast mask via single instruction
    if (len >= 16) {
        uint32_t m;
        memcpy(&m, mask, 4);
        uint8x16_t mask_vec = vreinterpretq_u8_u32(vdupq_n_u32(m));

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

std::string ws_build_upgrade_response(const char* sec_key, size_t key_len) {
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

    return resp;
}

// ══════════════════════════════════════════════════════════════════════════════
// Python-callable high-performance WebSocket functions
// ══════════════════════════════════════════════════════════════════════════════

// ── GIL-released frame parse helper ──────────────────────────────────────────
// Parses and unmasks frames into C++ structs. No Python API calls.
// ParsedFrameRef and ParseResult declared in ws_frame_parser.hpp

// ── Fragment assembler implementation ──────────────────────────────────────

int WsFragmentAssembler::feed(const WsFrame& frame, const uint8_t* unmasked_payload) {
    uint8_t opcode = (uint8_t)frame.opcode;

    if (opcode == WS_CONTINUATION) {
        if (!in_progress) {
            return -1;  // Protocol error: continuation without initial fragment
        }
        accumulated.insert(accumulated.end(),
            unmasked_payload, unmasked_payload + frame.payload_len);
        if (accumulated.size() > max_message_size) {
            reset();
            return -1;  // Message too large
        }
        if (frame.fin) {
            return 1;  // Complete message ready
        }
        return 0;  // Need more fragments
    }

    // Non-continuation data frame (TEXT or BINARY)
    if (opcode == WS_TEXT || opcode == WS_BINARY) {
        if (in_progress) {
            return -1;  // Protocol error: new data frame while assembling
        }
        if (!frame.fin) {
            // Start of fragmented message
            in_progress = true;
            original_opcode = opcode;
            accumulated.assign(unmasked_payload, unmasked_payload + frame.payload_len);
            return 0;  // Need more fragments
        }
        // Single complete frame — don't need assembler
        return 1;
    }

    // Control frames (PING/PONG/CLOSE) pass through — caller handles them
    return 1;
}

// ── GIL-released frame parser with optional fragment assembly ─────────────

void ws_parse_frames_nogil(uint8_t* data, size_t data_len, ParseResult& result,
                           WsFragmentAssembler* assembler) {
    size_t total_consumed = 0;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);
        if (consumed == 0) break;  // need more data
        if (consumed < 0) {
            // Protocol error (unmasked, reserved opcode, reserved RSV bits)
            result.protocol_error = consumed;
            break;
        }

        // Unmask in-place
        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        uint8_t opcode = (uint8_t)frame.opcode;

        // Control frames can be interleaved between fragments (RFC 6455 §5.4)
        if (opcode == WS_PING) {
            size_t pong_hdr = ws_frame_header_size((size_t)frame.payload_len);
            size_t old_size = result.pong_buf.size();
            result.pong_buf.resize(old_size + pong_hdr + (size_t)frame.payload_len);
            ws_write_frame_header(result.pong_buf.data() + old_size, WS_PONG, (size_t)frame.payload_len);
            if (frame.payload_len > 0) {
                memcpy(result.pong_buf.data() + old_size + pong_hdr, frame.payload, (size_t)frame.payload_len);
            }
            total_consumed += (size_t)consumed;
            continue;
        }

        if (opcode == WS_PONG) {
            result.pong_received = true;
            total_consumed += (size_t)consumed;
            continue;
        }

        if (opcode == WS_CLOSE) {
            result.frames.push_back({
                opcode,
                (size_t)(frame.payload - data),
                (size_t)frame.payload_len,
                false
            });
            total_consumed += (size_t)consumed;
            break;
        }

        // Data frames: handle fragmentation if assembler is provided
        if (assembler && (opcode == WS_CONTINUATION || !frame.fin)) {
            int asm_result = assembler->feed(frame, frame.payload);
            if (asm_result < 0) {
                result.protocol_error = -1;
                break;
            }
            if (asm_result == 1 && opcode == WS_CONTINUATION) {
                // Fragment reassembly complete — store as assembled message
                result.assembled.push_back({
                    assembler->original_opcode,
                    std::move(assembler->accumulated)
                });
                assembler->reset();
            }
            // asm_result == 0: need more fragments, or
            // asm_result == 1 && !CONTINUATION: single complete frame handled below
            if (asm_result == 0) {
                total_consumed += (size_t)consumed;
                continue;
            }
        }

        // Single complete frame — store reference
        result.frames.push_back({
            opcode,
            (size_t)(frame.payload - data),
            (size_t)frame.payload_len,
            false
        });

        total_consumed += (size_t)consumed;
    }

    result.total_consumed = total_consumed;
}

// ── ws_parse_frames(buffer: bytearray) -> (consumed, [(opcode, payload), ...], pong_bytes|None)
// Parses all complete frames from buffer in one C call with GIL release.
// Unmaskes payloads, auto-generates pong for ping frames.

PyObject* py_ws_parse_frames(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;

    // ── GIL released: parse + unmask all frames ──────────────────────────
    ParseResult pres;

    Py_BEGIN_ALLOW_THREADS
    ws_parse_frames_nogil(data, data_len, pres);
    Py_END_ALLOW_THREADS

    // ── GIL held: build Python objects from parsed frame refs ─────────────
    PyRef frames(PyList_New((Py_ssize_t)pres.frames.size()));
    if (!frames) { PyBuffer_Release(&buf); return nullptr; }

    for (size_t i = 0; i < pres.frames.size(); i++) {
        const auto& fr = pres.frames[i];
        PyObject* payload_bytes = PyBytes_FromStringAndSize(
            (const char*)(data + fr.payload_offset), (Py_ssize_t)fr.payload_len);
        if (!payload_bytes) { PyBuffer_Release(&buf); return nullptr; }

        PyObject* opcode_obj = PyLong_FromLong(fr.opcode);
        if (!opcode_obj) { Py_DECREF(payload_bytes); PyBuffer_Release(&buf); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_bytes);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_bytes);
        if (!tuple) { PyBuffer_Release(&buf); return nullptr; }

        PyList_SET_ITEM(frames.get(), (Py_ssize_t)i, tuple);  // steals ref
    }

    PyBuffer_Release(&buf);

    // Build result: (consumed, frames_list, pong_bytes_or_none)
    PyRef py_consumed(PyLong_FromSize_t(pres.total_consumed));

    PyObject* py_pong;
    if (!pres.pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize((const char*)pres.pong_buf.data(), (Py_ssize_t)pres.pong_buf.size());
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

    // For large payloads (>1KB), release GIL during memcpy to avoid blocking event loop
    if (payload_len > 1024) {
        uint8_t* tmp = (uint8_t*)PyMem_Malloc(total);
        if (!tmp) {
            PyBuffer_Release(&payload_buf);
            return PyErr_NoMemory();
        }
        const uint8_t* src = (const uint8_t*)payload_buf.buf;

        Py_BEGIN_ALLOW_THREADS
        ws_write_frame_header(tmp, (WsOpcode)opcode, payload_len);
        memcpy(tmp + hdr_size, src, payload_len);
        Py_END_ALLOW_THREADS

        PyBuffer_Release(&payload_buf);
        PyObject* result = PyBytes_FromStringAndSize((const char*)tmp, (Py_ssize_t)total);
        PyMem_Free(tmp);
        return result;
    }

    // Small payloads: allocate PyBytes directly — zero copy (GIL overhead not worth it)
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

// ── ws_build_ping_frame(payload: bytes|None) -> bytes

PyObject* py_ws_build_ping_frame(PyObject* /*self*/, PyObject* arg) {
    size_t plen = 0;
    const uint8_t* pdata = nullptr;
    Py_buffer payload_buf = {nullptr};

    if (arg != Py_None) {
        if (PyObject_GetBuffer(arg, &payload_buf, PyBUF_SIMPLE) < 0)
            return nullptr;
        if (payload_buf.len > 125) {
            PyBuffer_Release(&payload_buf);
            PyErr_SetString(PyExc_ValueError, "PING payload must be <= 125 bytes");
            return nullptr;
        }
        plen = (size_t)payload_buf.len;
        pdata = (const uint8_t*)payload_buf.buf;
    }

    size_t hdr_size = ws_frame_header_size(plen);
    size_t total = hdr_size + plen;

    PyObject* result = PyBytes_FromStringAndSize(nullptr, (Py_ssize_t)total);
    if (!result) {
        if (payload_buf.buf) PyBuffer_Release(&payload_buf);
        return nullptr;
    }

    uint8_t* out = (uint8_t*)PyBytes_AS_STRING(result);
    ws_write_frame_header(out, WS_PING, plen);
    if (plen > 0) memcpy(out + hdr_size, pdata, plen);

    if (payload_buf.buf) PyBuffer_Release(&payload_buf);
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
// Like ws_parse_frames but decodes TEXT frame payloads as UTF-8 str in C++.
// GIL released during parse + unmask, reacquired for Python object creation.

PyObject* py_ws_parse_frames_text(PyObject* /*self*/, PyObject* arg) {
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;

    // ── GIL released: parse + unmask all frames ──────────────────────────
    ParseResult pres;

    Py_BEGIN_ALLOW_THREADS
    ws_parse_frames_nogil(data, data_len, pres);
    Py_END_ALLOW_THREADS

    // ── GIL held: build Python objects with UTF-8 decode for TEXT frames ──
    PyRef frames(PyList_New((Py_ssize_t)pres.frames.size()));
    if (!frames) { PyBuffer_Release(&buf); return nullptr; }

    for (size_t i = 0; i < pres.frames.size(); i++) {
        const auto& fr = pres.frames[i];
        const char* payload_ptr = (const char*)(data + fr.payload_offset);

        PyObject* payload_obj;
        if (fr.opcode == WS_TEXT) {
            payload_obj = PyUnicode_DecodeUTF8(
                payload_ptr, (Py_ssize_t)fr.payload_len, "surrogateescape");
            if (!payload_obj) {
                // Fallback to bytes on decode error
                PyErr_Clear();
                payload_obj = PyBytes_FromStringAndSize(payload_ptr, (Py_ssize_t)fr.payload_len);
            }
        } else {
            payload_obj = PyBytes_FromStringAndSize(payload_ptr, (Py_ssize_t)fr.payload_len);
        }
        if (!payload_obj) { PyBuffer_Release(&buf); return nullptr; }

        PyObject* opcode_obj = PyLong_FromLong(fr.opcode);
        if (!opcode_obj) { Py_DECREF(payload_obj); PyBuffer_Release(&buf); return nullptr; }

        PyObject* tuple = PyTuple_Pack(2, opcode_obj, payload_obj);
        Py_DECREF(opcode_obj);
        Py_DECREF(payload_obj);
        if (!tuple) { PyBuffer_Release(&buf); return nullptr; }

        PyList_SET_ITEM(frames.get(), (Py_ssize_t)i, tuple);  // steals ref
    }

    PyBuffer_Release(&buf);

    PyRef py_consumed(PyLong_FromSize_t(pres.total_consumed));
    PyObject* py_pong;
    if (!pres.pong_buf.empty()) {
        py_pong = PyBytes_FromStringAndSize((const char*)pres.pong_buf.data(), (Py_ssize_t)pres.pong_buf.size());
        if (!py_pong) return nullptr;
    } else {
        Py_INCREF(Py_None);
        py_pong = Py_None;
    }

    return PyTuple_Pack(3, py_consumed.get(), frames.get(), py_pong);
}

// ── GIL-released echo helper ─────────────────────────────────────────────────
// Pure C++ work: parse frames, unmask, build echo output. No Python API calls.

// EchoResult struct declared in ws_frame_parser.hpp

void ws_echo_frames_nogil(
    uint8_t* data, size_t data_len,
    std::vector<uint8_t>& out, EchoResult& result)
{
    size_t total_consumed = 0;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);
        if (consumed == 0) break;   // need more data
        if (consumed < 0) break;    // protocol error — stop processing

        uint8_t opcode = (uint8_t)frame.opcode;

        if (opcode == WS_CLOSE) {
            result.has_close = true;
            if (frame.masked && frame.payload_len > 0) {
                ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
            }
            // Store offset+len for Python object creation after GIL reacquire
            result.close_payload_offset = (size_t)(frame.payload - data);
            result.close_payload_len = (size_t)frame.payload_len;
            total_consumed += (size_t)consumed;
            break;
        }

        if (opcode == WS_PONG) {
            total_consumed += (size_t)consumed;
            continue;
        }

        // Unmask payload in-place (SIMD-accelerated for large payloads)
        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        // Response opcode: PING → PONG, else echo same opcode
        WsOpcode resp_opcode = (opcode == WS_PING) ? WS_PONG : (WsOpcode)opcode;

        // Write response frame header
        uint8_t hdr[10];
        size_t hdr_len = ws_write_frame_header(hdr, resp_opcode, (size_t)frame.payload_len);

        // Append header + payload to output buffer
        size_t pos = out.size();
        out.resize(pos + hdr_len + (size_t)frame.payload_len);
        memcpy(out.data() + pos, hdr, hdr_len);
        if (frame.payload_len > 0) {
            memcpy(out.data() + pos + hdr_len, frame.payload, (size_t)frame.payload_len);
        }

        total_consumed += (size_t)consumed;
    }

    result.total_consumed = total_consumed;
}

// ── Buffer-target echo: writes directly into caller-provided buffer ──────────
// No std::vector allocation — writes echo frames directly into pre-allocated memory.
// Returns number of bytes written to out_buf.

size_t ws_echo_frames_into_buffer(
    uint8_t* out_buf, size_t out_cap,
    uint8_t* data, size_t data_len, EchoResult& result)
{
    size_t total_consumed = 0;
    size_t out_pos = 0;

    while (total_consumed < data_len) {
        WsFrame frame;
        int consumed = ws_parse_frame(data + total_consumed, data_len - total_consumed, &frame);
        if (consumed == 0) break;
        if (consumed < 0) break;

        uint8_t opcode = (uint8_t)frame.opcode;

        if (opcode == WS_CLOSE) {
            result.has_close = true;
            if (frame.masked && frame.payload_len > 0) {
                ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
            }
            result.close_payload_offset = (size_t)(frame.payload - data);
            result.close_payload_len = (size_t)frame.payload_len;
            total_consumed += (size_t)consumed;
            break;
        }

        if (opcode == WS_PONG) {
            total_consumed += (size_t)consumed;
            continue;
        }

        if (frame.masked && frame.payload_len > 0) {
            ws_unmask((uint8_t*)frame.payload, (size_t)frame.payload_len, frame.mask_key);
        }

        WsOpcode resp_opcode = (opcode == WS_PING) ? WS_PONG : (WsOpcode)opcode;

        uint8_t hdr[10];
        size_t hdr_len = ws_write_frame_header(hdr, resp_opcode, (size_t)frame.payload_len);
        size_t frame_total = hdr_len + (size_t)frame.payload_len;

        if (out_pos + frame_total > out_cap) break;  // buffer full

        memcpy(out_buf + out_pos, hdr, hdr_len);
        if (frame.payload_len > 0) {
            memcpy(out_buf + out_pos + hdr_len, frame.payload, (size_t)frame.payload_len);
        }
        out_pos += frame_total;

        total_consumed += (size_t)consumed;
    }

    result.total_consumed = total_consumed;
    return out_pos;
}

// ── ws_echo_frames(buffer: bytearray) -> (consumed, echo_bytes, close_payload|None)
// SINGLE-PASS with GIL release: Parse, unmask, and build echo response.
// Uses thread-local output buffer to avoid per-message heap allocation.
// Releases GIL during pure C++ work (parsing, SIMD unmasking, memcpy).

PyObject* py_ws_echo_frames(PyObject* /*self*/, PyObject* arg) {
    // ── GIL held: extract buffer ──────────────────────────────────────────
    Py_buffer buf;
    if (PyObject_GetBuffer(arg, &buf, PyBUF_WRITABLE) < 0) {
        return nullptr;
    }

    uint8_t* data = (uint8_t*)buf.buf;
    size_t data_len = (size_t)buf.len;

    // Thread-local output buffer — grows once during warmup, reused across calls
    static thread_local std::vector<uint8_t> out;
    out.clear();
    if (out.capacity() < data_len + 64) {
        out.reserve((data_len + 64) * 2);
    }

    EchoResult eres;

    // ── GIL released: pure C++ work (parsing + SIMD unmasking + memcpy) ──
    Py_BEGIN_ALLOW_THREADS
    ws_echo_frames_nogil(data, data_len, out, eres);
    Py_END_ALLOW_THREADS

    // ── GIL held: build Python objects ────────────────────────────────────

    // Build echo_bytes from thread-local buffer (single PyBytes allocation)
    PyObject* echo_bytes;
    if (!out.empty()) {
        echo_bytes = PyBytes_FromStringAndSize((const char*)out.data(), (Py_ssize_t)out.size());
        if (!echo_bytes) {
            PyBuffer_Release(&buf);
            return nullptr;
        }
    } else {
        Py_INCREF(Py_None);
        echo_bytes = Py_None;
    }

    // Build close payload (data still valid — Py_buffer not yet released)
    PyObject* close_obj;
    if (eres.has_close) {
        close_obj = PyBytes_FromStringAndSize(
            (const char*)(data + eres.close_payload_offset),
            (Py_ssize_t)eres.close_payload_len);
        if (!close_obj) {
            PyBuffer_Release(&buf);
            Py_DECREF(echo_bytes);
            return nullptr;
        }
    } else {
        Py_INCREF(Py_None);
        close_obj = Py_None;
    }

    PyBuffer_Release(&buf);

    PyRef py_consumed(PyLong_FromSize_t(eres.total_consumed));
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

// ══════════════════════════════════════════════════════════════════════════════
// Per-message compression (RFC 7692 permessage-deflate)
// ══════════════════════════════════════════════════════════════════════════════

#include <zlib.h>

bool WsDeflateContext::init() {
    auto* inf = new z_stream{};
    inf->zalloc = Z_NULL;
    inf->zfree = Z_NULL;
    inf->opaque = Z_NULL;
    if (inflateInit2(inf, -client_max_window_bits) != Z_OK) {
        delete inf;
        return false;
    }
    inflate_ctx = inf;

    auto* def = new z_stream{};
    def->zalloc = Z_NULL;
    def->zfree = Z_NULL;
    def->opaque = Z_NULL;
    // Level 1 (fastest) for WebSocket — most WS messages are small (<4KB)
    // and level 1 gives ~90% of the compression ratio at 3-5x the speed
    if (deflateInit2(def, 1, Z_DEFLATED,
                     -server_max_window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        inflateEnd(inf);
        delete inf;
        inflate_ctx = nullptr;
        delete def;
        return false;
    }
    deflate_ctx = def;
    enabled = true;
    return true;
}

void WsDeflateContext::destroy() {
    if (inflate_ctx) {
        inflateEnd(static_cast<z_stream*>(inflate_ctx));
        delete static_cast<z_stream*>(inflate_ctx);
        inflate_ctx = nullptr;
    }
    if (deflate_ctx) {
        deflateEnd(static_cast<z_stream*>(deflate_ctx));
        delete static_cast<z_stream*>(deflate_ctx);
        deflate_ctx = nullptr;
    }
    enabled = false;
}

bool WsDeflateContext::decompress(const uint8_t* in, size_t in_len,
                                  std::vector<uint8_t>& out) {
    if (!inflate_ctx) return false;
    auto* strm = static_cast<z_stream*>(inflate_ctx);

    out.clear();
    uint8_t buf[8192];
    int ret;

    // Step 1: Decompress the actual payload data (zero-copy — no input buffer allocation)
    strm->next_in = const_cast<uint8_t*>(in);
    strm->avail_in = (uInt)in_len;

    do {
        strm->next_out = buf;
        strm->avail_out = sizeof(buf);
        ret = inflate(strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            return false;
        }
        size_t have = sizeof(buf) - strm->avail_out;
        out.insert(out.end(), buf, buf + have);
    } while (strm->avail_out == 0);

    // Step 2: Feed the 4-byte RFC 7692 trailer separately (avoids copying entire input)
    static const uint8_t trailer[] = {0x00, 0x00, 0xFF, 0xFF};
    strm->next_in = const_cast<uint8_t*>(trailer);
    strm->avail_in = 4;

    do {
        strm->next_out = buf;
        strm->avail_out = sizeof(buf);
        ret = inflate(strm, Z_SYNC_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            return false;
        }
        size_t have = sizeof(buf) - strm->avail_out;
        out.insert(out.end(), buf, buf + have);
    } while (strm->avail_out == 0);

    if (client_no_context_takeover) {
        inflateReset(strm);
    }
    return true;
}

bool WsDeflateContext::compress(const uint8_t* in, size_t in_len,
                                std::vector<uint8_t>& out) {
    if (!deflate_ctx) return false;
    auto* strm = static_cast<z_stream*>(deflate_ctx);

    strm->next_in = const_cast<uint8_t*>(in);
    strm->avail_in = (uInt)in_len;

    out.clear();
    uint8_t buf[8192];
    int ret;
    do {
        strm->next_out = buf;
        strm->avail_out = sizeof(buf);
        ret = deflate(strm, Z_SYNC_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            return false;
        }
        size_t have = sizeof(buf) - strm->avail_out;
        out.insert(out.end(), buf, buf + have);
    } while (strm->avail_out == 0);

    // Per RFC 7692: remove trailing 0x00 0x00 0xFF 0xFF
    if (out.size() >= 4 &&
        out[out.size()-4] == 0x00 && out[out.size()-3] == 0x00 &&
        out[out.size()-2] == 0xFF && out[out.size()-1] == 0xFF) {
        out.resize(out.size() - 4);
    }

    if (server_no_context_takeover) {
        deflateReset(strm);
    }
    return true;
}

// ── OPT-11: Shared Deflate Context Pool ──────────────────────────────────
// Instead of allocating z_stream per connection (~500KB each), connections
// borrow from a shared pool when server_no_context_takeover is set.
// Pool size matches typical CPU core count for good cache locality.

#include <mutex>

namespace {

class SharedDeflatePool {
    static constexpr size_t POOL_SIZE = 16;

    struct PoolEntry {
        z_stream* inflate_ctx = nullptr;
        z_stream* deflate_ctx = nullptr;
        bool in_use = false;
    };

    PoolEntry entries_[POOL_SIZE];
    std::mutex mutex_;
    bool initialized_ = false;

public:
    SharedDeflatePool() = default;

    ~SharedDeflatePool() {
        for (auto& e : entries_) {
            if (e.inflate_ctx) { inflateEnd(e.inflate_ctx); delete e.inflate_ctx; }
            if (e.deflate_ctx) { deflateEnd(e.deflate_ctx); delete e.deflate_ctx; }
        }
    }

    // Initialize pool entries lazily on first acquire
    bool ensure_init() {
        if (initialized_) return true;
        for (size_t i = 0; i < POOL_SIZE; i++) {
            auto* inf = new z_stream{};
            inf->zalloc = Z_NULL; inf->zfree = Z_NULL; inf->opaque = Z_NULL;
            if (inflateInit2(inf, -15) != Z_OK) { delete inf; return false; }

            auto* def = new z_stream{};
            def->zalloc = Z_NULL; def->zfree = Z_NULL; def->opaque = Z_NULL;
            if (deflateInit2(def, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                inflateEnd(inf); delete inf; delete def; return false;
            }

            entries_[i].inflate_ctx = inf;
            entries_[i].deflate_ctx = def;
        }
        initialized_ = true;
        return true;
    }

    // Borrow a z_stream pair. Returns index or -1 if pool exhausted.
    int acquire(z_stream*& out_inflate, z_stream*& out_deflate) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_init()) return -1;
        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (!entries_[i].in_use) {
                entries_[i].in_use = true;
                out_inflate = entries_[i].inflate_ctx;
                out_deflate = entries_[i].deflate_ctx;
                return (int)i;
            }
        }
        return -1;  // pool exhausted — caller falls back to per-connection alloc
    }

    // Return a borrowed z_stream pair. Resets state for next use.
    void release(int idx) {
        if (idx < 0 || idx >= (int)POOL_SIZE) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = entries_[idx];
        if (e.inflate_ctx) inflateReset(e.inflate_ctx);
        if (e.deflate_ctx) deflateReset(e.deflate_ctx);
        e.in_use = false;
    }
};

static SharedDeflatePool s_deflate_pool;

} // anonymous namespace

// Public API: acquire/release shared deflate contexts
int shared_deflate_pool_acquire(void*& inflate_ctx, void*& deflate_ctx) {
    z_stream* inf = nullptr;
    z_stream* def = nullptr;
    int idx = s_deflate_pool.acquire(inf, def);
    if (idx >= 0) {
        inflate_ctx = inf;
        deflate_ctx = def;
    }
    return idx;
}

void shared_deflate_pool_release(int idx) {
    s_deflate_pool.release(idx);
}

// ── Upgrade response with extension negotiation ──────────────────────────

std::string ws_build_upgrade_response_ext(
    const char* sec_key, size_t key_len,
    const char* extensions, size_t ext_len,
    const char* subprotocol, size_t sub_len,
    WsDeflateContext* deflate_out)
{
    // Compute Sec-WebSocket-Accept
    SHA1 sha;
    sha.update((const uint8_t*)sec_key, key_len);
    sha.update((const uint8_t*)WS_MAGIC_GUID, sizeof(WS_MAGIC_GUID) - 1);
    uint8_t digest[20];
    sha.finalize(digest);
    std::string accept = base64_encode(digest, 20);

    // Check for permessage-deflate in extensions
    bool negotiate_deflate = false;
    if (extensions && ext_len > 0 && deflate_out) {
        std::string ext(extensions, ext_len);
        if (ext.find("permessage-deflate") != std::string::npos) {
            negotiate_deflate = true;
            // Parse parameters
            if (ext.find("server_no_context_takeover") != std::string::npos)
                deflate_out->server_no_context_takeover = true;
            if (ext.find("client_no_context_takeover") != std::string::npos)
                deflate_out->client_no_context_takeover = true;
            if (!deflate_out->init()) {
                negotiate_deflate = false;
            }
        }
    }

    // Build HTTP 101 response
    std::string resp;
    resp.reserve(512);
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n";

    if (negotiate_deflate) {
        resp += "Sec-WebSocket-Extensions: permessage-deflate";
        if (deflate_out->server_no_context_takeover)
            resp += "; server_no_context_takeover";
        if (deflate_out->client_no_context_takeover)
            resp += "; client_no_context_takeover";
        resp += "\r\n";
    }

    if (subprotocol && sub_len > 0) {
        resp += "Sec-WebSocket-Protocol: ";
        resp.append(subprotocol, sub_len);
        resp += "\r\n";
    }

    resp += "\r\n";
    return resp;
}
