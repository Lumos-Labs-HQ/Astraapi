#include "http_parser.hpp"

extern "C" {
#include "llhttp/llhttp.h"
}

#include <cstring>

// ── llhttp callback context ─────────────────────────────────────────────────
// Accumulates parse results into ParsedHttpRequest via llhttp callbacks.
// All string_views point into the original input buffer (zero-copy).

struct ParserContext {
    ParsedHttpRequest* out;
    const char* current_header_name;
    size_t current_header_name_len;
    bool headers_done;
    bool message_done;
};

// ── Callbacks ───────────────────────────────────────────────────────────────

static int on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    // llhttp may call on_url multiple times for chunked data — accumulate
    if (out->full_uri.data == nullptr) {
        out->full_uri = StringView(at, length);
    } else {
        // Extend: assumes contiguous buffer
        out->full_uri.len = (size_t)(at + length - out->full_uri.data);
    }
    return 0;
}

static int on_url_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    // Split full_uri into path and query_string at '?'
    const char* q = (const char*)memchr(out->full_uri.data, '?', out->full_uri.len);
    if (q) {
        out->path = StringView(out->full_uri.data, (size_t)(q - out->full_uri.data));
        out->query_string = StringView(q + 1, (size_t)(out->full_uri.data + out->full_uri.len - q - 1));
    } else {
        out->path = out->full_uri;
    }
    return 0;
}

static int on_method(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->method.data == nullptr) {
        out->method = StringView(at, length);
    } else {
        out->method.len = (size_t)(at + length - out->method.data);
    }
    return 0;
}

static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);

    if (ctx->current_header_name == nullptr) {
        ctx->current_header_name = at;
        ctx->current_header_name_len = length;
    } else {
        // Extend (contiguous)
        ctx->current_header_name_len = (size_t)(at + length - ctx->current_header_name);
    }
    return 0;
}

static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->header_count < MAX_HEADERS && ctx->current_header_name) {
        auto& hdr = out->headers[out->header_count];

        if (hdr.value.data == nullptr) {
            // First chunk — set both name and value
            hdr.name = StringView(ctx->current_header_name, ctx->current_header_name_len);
            hdr.value = StringView(at, length);
        } else {
            // Extend value (contiguous)
            hdr.value.len = (size_t)(at + length - hdr.value.data);
        }
    }
    return 0;
}

static int on_header_value_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->header_count < MAX_HEADERS) {
        out->header_count++;
    }
    ctx->current_header_name = nullptr;
    ctx->current_header_name_len = 0;
    return 0;
}

static int on_headers_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    // Extract connection info from llhttp flags
    out->keep_alive = llhttp_should_keep_alive(parser);
    out->upgrade = (parser->upgrade != 0);
    out->chunked = (parser->flags & F_CHUNKED) != 0;

    ctx->headers_done = true;

    // If upgrade, pause the parser (WebSocket etc.)
    if (out->upgrade) {
        return 2;  // Tell llhttp to pause for upgrade
    }
    return 0;
}

static int on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->chunked) {
        // Chunked TE: accumulate body chunks into reassembly buffer (lazy alloc)
        out->append_chunked(at, length);
    } else {
        // Content-Length: zero-copy pointer into input buffer
        if (out->body.data == nullptr) {
            out->body = StringView(at, length);
        } else {
            // Extend (contiguous buffer)
            out->body.len = (size_t)(at + length - out->body.data);
        }
    }
    return 0;
}

static int on_message_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    ctx->message_done = true;

    // Pause after one complete message (we process one request at a time)
    llhttp_pause(parser);
    return HPE_PAUSED;
}

// ── Static settings (initialized once) ──────────────────────────────────────
static llhttp_settings_t s_settings;
static bool s_settings_init = false;

static void ensure_settings() {
    if (s_settings_init) return;
    llhttp_settings_init(&s_settings);
    s_settings.on_method = on_method;
    s_settings.on_url = on_url;
    s_settings.on_url_complete = on_url_complete;
    s_settings.on_header_field = on_header_field;
    s_settings.on_header_value = on_header_value;
    s_settings.on_header_value_complete = on_header_value_complete;
    s_settings.on_headers_complete = on_headers_complete;
    s_settings.on_body = on_body;
    s_settings.on_message_complete = on_message_complete;
    s_settings_init = true;
}

// ── Main parser entry point ─────────────────────────────────────────────────

int parse_http_request(const char* data, size_t len, ParsedHttpRequest* out) {
    ensure_settings();

    // Zero-init output
    out->method = StringView();
    out->path = StringView();
    out->query_string = StringView();
    out->full_uri = StringView();
    out->header_count = 0;
    out->body = StringView();
    out->keep_alive = true;
    out->upgrade = false;
    out->chunked = false;
    out->total_consumed = 0;
    out->chunked_body_ptr = nullptr;  // lazy — only allocated for chunked requests
    // Zero-init header value pointers (for chunked header value detection)
    for (int i = 0; i < MAX_HEADERS; i++) {
        out->headers[i].value.data = nullptr;
        out->headers[i].value.len = 0;
    }

    // Thread-local parser reuse: llhttp_reset() is ~10x cheaper than llhttp_init()
    // (resets state without re-assigning 15 callback function pointers)
    static thread_local bool tl_parser_inited = false;
    static thread_local llhttp_t tl_parser;
    if (!tl_parser_inited) {
        llhttp_init(&tl_parser, HTTP_REQUEST, &s_settings);
        tl_parser_inited = true;
    } else {
        llhttp_reset(&tl_parser);
    }
    llhttp_t& parser = tl_parser;

    ParserContext ctx;
    ctx.out = out;
    ctx.current_header_name = nullptr;
    ctx.current_header_name_len = 0;
    ctx.headers_done = false;
    ctx.message_done = false;
    parser.data = &ctx;

    // Execute parser
    llhttp_errno_t err = llhttp_execute(&parser, data, len);

    if (__builtin_expect(!!(err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE), 1)) {
        // Message complete or upgrade detected
        // Calculate consumed bytes
        const char* error_pos = llhttp_get_error_pos(&parser);
        if (error_pos) {
            out->total_consumed = (size_t)(error_pos - data);
        } else {
            out->total_consumed = len;
        }

        // For chunked: point body into reassembled buffer
        if (out->chunked && out->chunked_body_ptr && !out->chunked_body_ptr->empty()) {
            out->body = StringView(out->chunked_body_ptr->data(), out->chunked_body_ptr->size());
        }

        return 1;  // Complete request
    }

    if (err == HPE_OK) {
        // Parser consumed everything but message not yet complete — need more data
        if (!ctx.message_done) {
            return 0;
        }
        out->total_consumed = len;

        if (out->chunked && out->chunked_body_ptr && !out->chunked_body_ptr->empty()) {
            out->body = StringView(out->chunked_body_ptr->data(), out->chunked_body_ptr->size());
        }

        return 1;
    }

    // Parse error
    return -1;
}
