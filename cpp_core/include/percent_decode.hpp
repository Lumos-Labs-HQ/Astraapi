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
