#pragma once

// ── Platform abstraction layer ──────────────────────────────────────────────
// Provides portable alternatives to POSIX-only functions/types.
// Include this instead of <strings.h> or <unistd.h>.

#ifdef _WIN32
    #include <io.h>
    #include <winsock2.h>
    #include <BaseTsd.h>
    #include <string.h>

    using ssize_t = SSIZE_T;

    // POSIX strcasecmp/strncasecmp → MSVC _stricmp/_strnicmp
    inline int strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
    inline int strncasecmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }

    // Socket write: Windows uses send() not write()
    inline ssize_t platform_socket_write(int fd, const void* buf, size_t len) {
        return send(static_cast<SOCKET>(fd), static_cast<const char*>(buf),
                    static_cast<int>(len), 0);
    }
#else
    #include <strings.h>
    #include <unistd.h>

    inline ssize_t platform_socket_write(int fd, const void* buf, size_t len) {
        return write(fd, buf, len);
    }
#endif
