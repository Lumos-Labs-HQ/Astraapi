#pragma once

// ── Platform abstraction layer ──────────────────────────────────────────────
// Provides portable alternatives to POSIX-only functions/types.
// Include this instead of <strings.h> or <unistd.h>.

#include <cstddef>
#include <cstring>

// ── SIMD small memcpy (OPT-18) ─────────────────────────────────────────────
// For copies < 64 bytes, inline SSE2/NEON stores are faster than function call
// overhead of standard memcpy. Used for WS frame headers, small payloads,
// HTTP status line copies.

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
inline void fast_memcpy_small(void* dst, const void* src, size_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (len >= 32) {
        _mm_storeu_si128((__m128i*)d, _mm_loadu_si128((__m128i const*)s));
        _mm_storeu_si128((__m128i*)(d + 16), _mm_loadu_si128((__m128i const*)(s + 16)));
        if (len > 32) memcpy(d + 32, s + 32, len - 32);
    } else if (len >= 16) {
        _mm_storeu_si128((__m128i*)d, _mm_loadu_si128((__m128i const*)s));
        if (len > 16) memcpy(d + 16, s + 16, len - 16);
    } else if (len >= 8) {
        uint64_t v;
        memcpy(&v, s, 8);
        memcpy(d, &v, 8);
        if (len > 8) { memcpy(&v, s + len - 8, 8); memcpy(d + len - 8, &v, 8); }
    } else if (len >= 4) {
        uint32_t v;
        memcpy(&v, s, 4);
        memcpy(d, &v, 4);
        if (len > 4) { memcpy(&v, s + len - 4, 4); memcpy(d + len - 4, &v, 4); }
    } else {
        for (size_t i = 0; i < len; i++) d[i] = s[i];
    }
}
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
inline void fast_memcpy_small(void* dst, const void* src, size_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (len >= 32) {
        vst1q_u8(d, vld1q_u8(s));
        vst1q_u8(d + 16, vld1q_u8(s + 16));
        if (len > 32) memcpy(d + 32, s + 32, len - 32);
    } else if (len >= 16) {
        vst1q_u8(d, vld1q_u8(s));
        if (len > 16) memcpy(d + 16, s + 16, len - 16);
    } else {
        memcpy(dst, src, len);
    }
}
#else
inline void fast_memcpy_small(void* dst, const void* src, size_t len) {
    memcpy(dst, src, len);
}
#endif

// ── Scatter-gather I/O vector (platform-independent) ────────────────────────
// Layout matches POSIX struct iovec {void* iov_base; size_t iov_len}
struct PlatformIoVec {
    void* base;
    size_t len;
};

#ifdef _WIN32
    #include <io.h>
    #include <winsock2.h>
    #include <BaseTsd.h>
    #include <string.h>
    #include <cstdlib>

    using ssize_t = SSIZE_T;

    // POSIX strcasecmp/strncasecmp → MSVC _stricmp/_strnicmp
    inline int strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
    inline int strncasecmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }

    // Socket write: Windows uses send() not write()
    inline ssize_t platform_socket_write(int fd, const void* buf, size_t len) {
        return send(static_cast<SOCKET>(fd), static_cast<const char*>(buf),
                    static_cast<int>(len), 0);
    }

    // Scatter-gather write: Windows uses WSASend with WSABUF array
    inline ssize_t platform_socket_writev(int fd, PlatformIoVec* iov, int iovcnt) {
        WSABUF stack_bufs[64];
        WSABUF* bufs = (iovcnt <= 64) ? stack_bufs
            : static_cast<WSABUF*>(malloc(static_cast<size_t>(iovcnt) * sizeof(WSABUF)));
        if (!bufs) return -1;
        for (int i = 0; i < iovcnt; i++) {
            bufs[i].buf = static_cast<char*>(iov[i].base);
            bufs[i].len = static_cast<ULONG>(iov[i].len);
        }
        DWORD bytes_sent = 0;
        int result = WSASend(static_cast<SOCKET>(fd), bufs, iovcnt,
                             &bytes_sent, 0, nullptr, nullptr);
        if (bufs != stack_bufs) free(bufs);
        if (result == SOCKET_ERROR) return -1;
        return static_cast<ssize_t>(bytes_sent);
    }
#else
    #include <strings.h>
    #include <unistd.h>
    #include <sys/uio.h>

    inline ssize_t platform_socket_write(int fd, const void* buf, size_t len) {
        return write(fd, buf, len);
    }

    // Scatter-gather write: POSIX writev() — zero-copy cast since PlatformIoVec
    // has identical layout to struct iovec {void* iov_base; size_t iov_len}
    inline ssize_t platform_socket_writev(int fd, PlatformIoVec* iov, int iovcnt) {
        static_assert(sizeof(PlatformIoVec) == sizeof(struct iovec),
                      "PlatformIoVec must match struct iovec layout");
        return writev(fd, reinterpret_cast<struct iovec*>(iov), iovcnt);
    }
#endif
