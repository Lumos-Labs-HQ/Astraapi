#pragma once
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

// ── Zero-copy HTTP/1.1 request parser (llhttp-backed) ──────────────────────
// All string_views point into the input buffer — no heap allocations.
// Designed for asyncio.Protocol data_received() → C++ handle_http() fast path.

struct StringView {
    const char* data;
    size_t len;

    StringView() : data(nullptr), len(0) {}
    StringView(const char* d, size_t l) : data(d), len(l) {}

    bool empty() const { return len == 0; }

    bool equals(const char* s, size_t slen) const {
        return len == slen && memcmp(data, s, slen) == 0;
    }

    // Case-insensitive compare (for header names)
    bool iequals(const char* s, size_t slen) const {
        if (len != slen) return false;
        for (size_t i = 0; i < len; i++) {
            char a = data[i];
            char b = s[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != b) return false;
        }
        return true;
    }
};

struct HttpHeader {
    StringView name;
    StringView value;
};

static constexpr int MAX_HEADERS = 64;

struct ParsedHttpRequest {
    StringView method;          // "GET", "POST", etc.
    StringView path;            // "/items/42" (before '?')
    StringView query_string;    // "q=test&page=1" (after '?', without '?')
    StringView full_uri;        // "/items/42?q=test&page=1"

    HttpHeader headers[MAX_HEADERS];
    int header_count;

    StringView body;            // Request body (Content-Length or reassembled chunked)
    bool keep_alive;            // Connection: keep-alive (default true for HTTP/1.1)
    bool upgrade;               // Connection: Upgrade (WebSocket, h2c, etc.)
    bool chunked;               // Transfer-Encoding: chunked (body is reassembled)
    size_t total_consumed;      // Total bytes consumed from input

    // For chunked TE: reassembled body buffer (body.data points into this)
    std::vector<char> chunked_body;

    // Find a header by lowercase name
    StringView find_header(const char* name, size_t name_len) const {
        for (int i = 0; i < header_count; i++) {
            if (headers[i].name.iequals(name, name_len)) {
                return headers[i].value;
            }
        }
        return StringView();
    }
};

// Parse one HTTP/1.1 request from buffer.
// Returns:  1 = complete request parsed
//           0 = need more data (incomplete headers or body)
//          -1 = parse error (malformed request)
int parse_http_request(const char* data, size_t len, ParsedHttpRequest* out);
