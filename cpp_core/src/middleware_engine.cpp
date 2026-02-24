#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "pyref.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

// libdeflate (optional — 2-3x faster single-shot gzip than zlib)
#ifndef HAS_LIBDEFLATE
#define HAS_LIBDEFLATE 0
#endif
#if HAS_LIBDEFLATE
#include <libdeflate.h>
#endif

// Brotli headers (optional — CMake defines HAS_BROTLI=1 if found)
#if HAS_BROTLI
#include <brotli/encode.h>
#include <brotli/decode.h>
#endif
#ifndef HAS_BROTLI
#define HAS_BROTLI 0
#endif

// ── Deduplicated case-insensitive helpers (file-scope static inline) ─────────
// Previously duplicated as lambdas inside 3 different functions.

static inline bool ci_starts_mw(const char* s, size_t s_len, const char* prefix, size_t p_len) {
    if (s_len < p_len) return false;
    for (size_t i = 0; i < p_len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != prefix[i]) return false;
    }
    return true;
}

static inline bool ci_contains_mw(const char* s, size_t s_len, const char* needle, size_t n_len) {
    if (s_len < n_len) return false;
    for (size_t i = 0; i <= s_len - n_len; i++) {
        bool match = true;
        for (size_t j = 0; j < n_len; j++) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needle[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// ── Pre-interned encoding strings (eliminates PyUnicode_FromString per call) ─
static PyObject* s_enc_gzip = nullptr;
static PyObject* s_enc_br = nullptr;
static PyObject* s_enc_identity = nullptr;

static void ensure_interned_encodings() {
    if (!s_enc_gzip) {
        s_enc_gzip = PyUnicode_InternFromString("gzip");
        s_enc_br = PyUnicode_InternFromString("br");
        s_enc_identity = PyUnicode_InternFromString("identity");
    }
}

// ── Thread-local compression output buffer (avoids malloc/free per response) ─
static thread_local std::vector<uint8_t> tl_compress_buf;

// ══════════════════════════════════════════════════════════════════════════════
// gzip_compress(data: bytes, level: int = 6) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_gzip_compress(PyObject* self, PyObject* args) {
    PyObject* data_obj;
    int level = 4;  // Level 4: ~30-40% less CPU than 6 for ~2-3% larger output
    if (!PyArg_ParseTuple(args, "O|i", &data_obj, &level)) return nullptr;

    if (!PyBytes_Check(data_obj)) {
        PyErr_SetString(PyExc_TypeError, "expected bytes");
        return nullptr;
    }

    char* data;
    Py_ssize_t data_len;
    PyBytes_AsStringAndSize(data_obj, &data, &data_len);

#if HAS_LIBDEFLATE
    // libdeflate fast-path: 2-3x faster single-shot gzip compression
    // Uses thread-local buffer to avoid malloc/free per response
    tl_compress_buf.clear();
    size_t actual_size = 0;
    bool ok = false;

    Py_BEGIN_ALLOW_THREADS
    struct libdeflate_compressor* c = libdeflate_alloc_compressor(level);
    if (c) {
        size_t bound = libdeflate_gzip_compress_bound(c, (size_t)data_len);
        tl_compress_buf.resize(bound);
        actual_size = libdeflate_gzip_compress(c, data, (size_t)data_len,
                                                tl_compress_buf.data(), tl_compress_buf.size());
        libdeflate_free_compressor(c);
        ok = (actual_size > 0);
    }
    Py_END_ALLOW_THREADS

    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "gzip compression failed (libdeflate)");
        return nullptr;
    }
    return PyBytes_FromStringAndSize((const char*)tl_compress_buf.data(), (Py_ssize_t)actual_size);
#else
    // zlib fallback — thread-local buffer
    tl_compress_buf.clear();
    int zret;

    Py_BEGIN_ALLOW_THREADS

    z_stream strm = {};
    // windowBits = 15 + 16 for gzip format
    zret = deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (zret == Z_OK) {
        tl_compress_buf.resize(deflateBound(&strm, (uLong)data_len));
        strm.next_in = (Bytef*)data;
        strm.avail_in = (uInt)data_len;
        strm.next_out = tl_compress_buf.data();
        strm.avail_out = (uInt)tl_compress_buf.size();

        zret = deflate(&strm, Z_FINISH);
        if (zret == Z_STREAM_END) {
            tl_compress_buf.resize(strm.total_out);
        }
        deflateEnd(&strm);
    }

    Py_END_ALLOW_THREADS

    if (zret != Z_STREAM_END && zret != Z_OK) {
        PyErr_SetString(PyExc_RuntimeError, "gzip compression failed");
        return nullptr;
    }

    return PyBytes_FromStringAndSize((const char*)tl_compress_buf.data(), (Py_ssize_t)tl_compress_buf.size());
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// gzip_decompress(data: bytes) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_gzip_decompress(PyObject* self, PyObject* args) {
    PyObject* data_obj;
    if (!PyArg_ParseTuple(args, "O", &data_obj)) return nullptr;

    if (!PyBytes_Check(data_obj)) {
        PyErr_SetString(PyExc_TypeError, "expected bytes");
        return nullptr;
    }

    char* data;
    Py_ssize_t data_len;
    PyBytes_AsStringAndSize(data_obj, &data, &data_len);

    std::vector<uint8_t> output;
    int zret;
    static constexpr size_t MAX_DECOMPRESSED = 256 * 1024 * 1024;  // 256 MB

    Py_BEGIN_ALLOW_THREADS

    z_stream strm = {};
    // windowBits = 15 + 32 for auto-detect gzip/zlib
    zret = inflateInit2(&strm, 15 + 32);
    if (zret == Z_OK) {
        // Smarter initial size: cap at 16MB to avoid overalloc on small inputs
        size_t initial = std::min((size_t)data_len * 4, (size_t)(16 * 1024 * 1024));
        if (initial < 4096) initial = 4096;
        output.resize(initial);
        strm.next_in = (Bytef*)data;
        strm.avail_in = (uInt)data_len;

        while (true) {
            strm.next_out = output.data() + strm.total_out;
            strm.avail_out = (uInt)(output.size() - strm.total_out);

            zret = inflate(&strm, Z_NO_FLUSH);
            if (zret == Z_STREAM_END) break;
            if (zret != Z_OK) break;
            if (strm.avail_out == 0) {
                size_t new_size = output.size() * 2;
                if (new_size > MAX_DECOMPRESSED) {
                    zret = Z_MEM_ERROR;
                    break;
                }
                output.resize(new_size);
            }
        }
        output.resize(strm.total_out);
        inflateEnd(&strm);
    }

    Py_END_ALLOW_THREADS

    if (zret != Z_STREAM_END) {
        PyErr_SetString(PyExc_RuntimeError, "gzip decompression failed");
        return nullptr;
    }

    return PyBytes_FromStringAndSize((const char*)output.data(), (Py_ssize_t)output.size());
}

// ══════════════════════════════════════════════════════════════════════════════
// brotli_compress(data: bytes, quality: int = 4) → PyBytes
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_brotli_compress(PyObject* self, PyObject* args) {
    PyObject* data_obj;
    int quality = 4;
    if (!PyArg_ParseTuple(args, "O|i", &data_obj, &quality)) return nullptr;

    if (!PyBytes_Check(data_obj)) {
        PyErr_SetString(PyExc_TypeError, "expected bytes");
        return nullptr;
    }

    char* data;
    Py_ssize_t data_len;
    PyBytes_AsStringAndSize(data_obj, &data, &data_len);

#if HAS_BROTLI
    size_t output_size = BrotliEncoderMaxCompressedSize((size_t)data_len);
    if (output_size == 0) output_size = (size_t)data_len + 1024;

    // Thread-local buffer for brotli output
    tl_compress_buf.clear();
    tl_compress_buf.resize(output_size);
    int ok;

    Py_BEGIN_ALLOW_THREADS

    ok = BrotliEncoderCompress(
        quality, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
        (size_t)data_len, (const uint8_t*)data,
        &output_size, tl_compress_buf.data());

    Py_END_ALLOW_THREADS

    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError, "brotli compression failed");
        return nullptr;
    }

    return PyBytes_FromStringAndSize((const char*)tl_compress_buf.data(), (Py_ssize_t)output_size);
#else
    // Fallback: return gzip compressed
    return py_gzip_compress(self, args);
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// negotiate_encoding(accept_encoding: str) → str
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_negotiate_encoding(PyObject* self, PyObject* arg) {
    const char* ae;
    Py_ssize_t ae_len;
    if (PyUnicode_Check(arg)) {
        ae = PyUnicode_AsUTF8AndSize(arg, &ae_len);
        if (!ae) return nullptr;
    } else {
        PyErr_SetString(PyExc_TypeError, "expected str");
        return nullptr;
    }

    ensure_interned_encodings();

#if HAS_BROTLI
    if (ci_contains_mw(ae, ae_len, "br", 2)) {
        Py_INCREF(s_enc_br);
        return s_enc_br;
    }
#endif
    if (ci_contains_mw(ae, ae_len, "gzip", 4)) {
        Py_INCREF(s_enc_gzip);
        return s_enc_gzip;
    }

    Py_INCREF(s_enc_identity);
    return s_enc_identity;
}

// ══════════════════════════════════════════════════════════════════════════════
// should_compress(content_type: str, body_size: int, min_size: int = 500) → bool
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_should_compress(PyObject* self, PyObject* args) {
    const char* content_type;
    Py_ssize_t body_size;
    Py_ssize_t min_size = 500;

    if (!PyArg_ParseTuple(args, "sn|n", &content_type, &body_size, &min_size)) return nullptr;

    if (body_size < min_size) Py_RETURN_FALSE;

    size_t ct_len = strlen(content_type);

    if (ci_starts_mw(content_type, ct_len, "text/", 5) ||
        ci_contains_mw(content_type, ct_len, "application/json", 16) ||
        ci_contains_mw(content_type, ct_len, "application/xml", 15) ||
        ci_contains_mw(content_type, ct_len, "application/javascript", 22) ||
        ci_contains_mw(content_type, ct_len, "+json", 5) ||
        ci_contains_mw(content_type, ct_len, "+xml", 4)) {
        Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
}

// ══════════════════════════════════════════════════════════════════════════════
// compress_response(body, accept_encoding, content_type, min_size, gzip_level, brotli_quality)
// → Optional[Tuple[PyBytes, str]]
// ══════════════════════════════════════════════════════════════════════════════

PyObject* py_compress_response(PyObject* self, PyObject* args) {
    PyObject* body_obj;
    const char* accept_encoding;
    const char* content_type;
    Py_ssize_t min_size = 500;
    int gzip_level = 4;
    int brotli_quality = 4;

    if (!PyArg_ParseTuple(args, "Oss|nii",
            &body_obj, &accept_encoding, &content_type,
            &min_size, &gzip_level, &brotli_quality)) return nullptr;

    if (!PyBytes_Check(body_obj)) {
        PyErr_SetString(PyExc_TypeError, "expected bytes body");
        return nullptr;
    }

    Py_ssize_t body_size = PyBytes_GET_SIZE(body_obj);

    // Check if we should compress
    if (body_size < min_size) Py_RETURN_NONE;

    size_t ct_len = strlen(content_type);
    bool compressible = (ci_starts_mw(content_type, ct_len, "text/", 5) ||
                         ci_contains_mw(content_type, ct_len, "application/json", 16) ||
                         ci_contains_mw(content_type, ct_len, "application/xml", 15) ||
                         ci_contains_mw(content_type, ct_len, "application/javascript", 22) ||
                         ci_contains_mw(content_type, ct_len, "+json", 5) ||
                         ci_contains_mw(content_type, ct_len, "+xml", 4));
    if (!compressible) Py_RETURN_NONE;

    // Negotiate encoding — zero-alloc case-insensitive search
    size_t ae_len = strlen(accept_encoding);

    const char* encoding = nullptr;
    PyObject* compressed = nullptr;

#if HAS_BROTLI
    if (ci_contains_mw(accept_encoding, ae_len, "br", 2)) {
        encoding = "br";
        PyRef br_args(Py_BuildValue("(Oi)", body_obj, brotli_quality));
        compressed = py_brotli_compress(self, br_args.get());
    } else
#endif
    if (ci_contains_mw(accept_encoding, ae_len, "gzip", 4)) {
        encoding = "gzip";
        PyRef gz_args(Py_BuildValue("(Oi)", body_obj, gzip_level));
        compressed = py_gzip_compress(self, gz_args.get());
    }

    if (!compressed || !encoding) Py_RETURN_NONE;
    if (PyErr_Occurred()) { Py_XDECREF(compressed); return nullptr; }

    ensure_interned_encodings();
    PyObject* enc_str = (strcmp(encoding, "br") == 0) ? s_enc_br : s_enc_gzip;
    Py_INCREF(enc_str);
    PyObject* result = PyTuple_Pack(2, compressed, enc_str);
    Py_DECREF(compressed);
    return result;
}
