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
    // Optimized: early-exit on length mismatch + first-byte check before full scan
    bool iequals(const char* s, size_t slen) const {
        if (len != slen) return false;
        if (len == 0) return true;
        // First-byte fast reject (lowercase both)
        char a0 = data[0]; if (a0 >= 'A' && a0 <= 'Z') a0 += 32;
        if (a0 != s[0]) return false;
        for (size_t i = 1; i < len; i++) {
            char a = data[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (a != s[i]) return false;
        }
        return true;
    }
};

struct HttpHeader {
    StringView name;
    StringView value;
};

static constexpr int MAX_HEADERS = 32;

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
    bool body_too_large;        // Set true if chunked body exceeds MAX_CHUNKED_BODY
    size_t total_consumed;      // Total bytes consumed from input

    // Max chunked body size: 10 MB (prevents OOM from malicious chunked streams)
    static constexpr size_t MAX_CHUNKED_BODY = 10 * 1024 * 1024;

    // For chunked TE: reassembled body buffer (body.data points into this)
    // Lazy: only allocated for chunked requests (99%+ of requests skip this)
    std::vector<char>* chunked_body_ptr = nullptr;

    void append_chunked(const char* at, size_t length) {
        if (!chunked_body_ptr) {
            // Thread-local scratch buffer avoids malloc/free per-request
            static thread_local std::vector<char> tl_chunked;
            tl_chunked.clear();
            tl_chunked.reserve(4096);
            chunked_body_ptr = &tl_chunked;
        }
        if (chunked_body_ptr->size() + length > MAX_CHUNKED_BODY) {
            body_too_large = true;
            return;
        }
        size_t old = chunked_body_ptr->size();
        chunked_body_ptr->resize(old + length);
        std::memcpy(chunked_body_ptr->data() + old, at, length);
    }

    // Find a header by lowercase name
    // Optimized: length-dispatch + first-byte check eliminates most comparisons
    StringView find_header(const char* name, size_t name_len) const {
        char first = (name_len > 0) ? name[0] : 0;  // name is already lowercase
        for (int i = 0; i < header_count; i++) {
            if (headers[i].name.len != name_len) continue;  // length mismatch: skip
            // First-byte fast check (lowercase both)
            char h0 = headers[i].name.data[0];
            if (h0 >= 'A' && h0 <= 'Z') h0 += 32;
            if (h0 != first) continue;  // first byte mismatch: skip
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
