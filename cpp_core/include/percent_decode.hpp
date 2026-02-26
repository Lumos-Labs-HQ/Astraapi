#pragma once
#include <string>
#include <cstddef>

// Shared percent-decoding for query strings, URL-encoded bodies, and request parsing.
// Also converts '+' to space (form-encoded convention).

inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string percent_decode(const char* s, size_t len);

// Reuse-buffer variant: clears & writes into caller's pre-allocated string.
// Avoids per-call heap allocation when used in a loop with scratch buffers.
inline void percent_decode_into(std::string& out, const char* s, size_t len) {
    out.clear();
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
}

// Fast check: does the string_view contain any percent-encoded or '+' chars?
// Returns false if the string is already decoded (common case), avoiding decode overhead.
inline bool needs_percent_decode(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' || s[i] == '+') return true;
    }
    return false;
}
