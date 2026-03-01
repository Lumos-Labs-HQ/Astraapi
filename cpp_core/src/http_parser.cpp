#include "http_parser.hpp"
#include "compat.hpp"

extern "C" {
#include "llhttp/llhttp.h"
}

#include <cstring>

struct ParserContext {
    ParsedHttpRequest* out;
    const char* current_header_name;
    size_t current_header_name_len;
    bool headers_done;
    bool message_done;
};

static int on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->full_uri.data == nullptr) {
        out->full_uri = StringView(at, length);
    } else {
        out->full_uri.len = static_cast<size_t>(at + length - out->full_uri.data);
    }

    return 0;
}

static int on_url_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    const char* q = static_cast<const char*>(
        memchr(out->full_uri.data, '?', out->full_uri.len)
    );

    if (q) {
        out->path = StringView(out->full_uri.data, q - out->full_uri.data);
        out->query_string = StringView(
            q + 1,
            out->full_uri.data + out->full_uri.len - q - 1
        );
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
        out->method.len = static_cast<size_t>(at + length - out->method.data);
    }

    return 0;
}

static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);

    if (!ctx->current_header_name) {
        ctx->current_header_name = at;
        ctx->current_header_name_len = length;
    } else {
        ctx->current_header_name_len =
            static_cast<size_t>(at + length - ctx->current_header_name);
    }

    return 0;
}

static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    if (out->header_count < MAX_HEADERS && ctx->current_header_name) {
        auto& hdr = out->headers[out->header_count];

        if (hdr.value.data == nullptr) {
            hdr.name = StringView(
                ctx->current_header_name,
                ctx->current_header_name_len
            );
            hdr.value = StringView(at, length);
        } else {
            hdr.value.len =
                static_cast<size_t>(at + length - hdr.value.data);
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

    out->keep_alive = llhttp_should_keep_alive(parser);
    out->upgrade = parser->upgrade != 0;

    ctx->headers_done = true;

    if (out->upgrade) {
        return 2;
    }

    // Use llhttp's pre-parsed method enum — O(1) integer comparison.
    const uint8_t m = parser->method;
    if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) {
        out->no_body = true;
        out->is_head = (m == HTTP_HEAD);
        out->chunked = false;  // suppress chunked reassembly path

        // Save declared body size before F_SKIPBODY clears state.
        // UINT64_MAX = no Content-Length header present.
        out->body_skip_len = parser->content_length;

        // F_SKIPBODY tells llhttp to fire message_complete immediately
        // after headers WITHOUT entering the body read state machine.
        // This means llhttp_execute returns HPE_PAUSED at the header
        // boundary, not after waiting for all body bytes to arrive.
        //
        // Only set for non-chunked. Chunked GET bodies fall back to the
        // on_body no-op path (chunked framing must be fully consumed
        // to keep the stream aligned, and we can't know chunk total upfront).
        if (!(parser->flags & F_CHUNKED)) {
            parser->flags |= F_SKIPBODY;
        }
        return 0;
    }

    out->chunked = (parser->flags & F_CHUNKED) != 0;
    return 0;
}

static int on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = static_cast<ParserContext*>(parser->data);
    auto* out = ctx->out;

    // GET/HEAD/OPTIONS non-chunked: F_SKIPBODY was set — on_body is never
    // called by llhttp. This branch handles GET with chunked TE (rare):
    // chunked framing must be consumed by llhttp but we discard bytes.
    if (out->no_body) return 0;

    if (out->chunked) {
        out->append_chunked(at, length);
    } else {
        if (out->body.data == nullptr) {
            out->body = StringView(at, length);
        } else {
            out->body.len =
                static_cast<size_t>(at + length - out->body.data);
        }
    }

    return 0;
}

static int on_message_complete(llhttp_t* parser) {
    auto* ctx = static_cast<ParserContext*>(parser->data);

    ctx->message_done = true;
    llhttp_pause(parser);

    return HPE_PAUSED;
}

static llhttp_settings_t settings;
static bool settings_init = false;

static void ensure_settings() {
    if (settings_init)
        return;

    llhttp_settings_init(&settings);

    settings.on_method = on_method;
    settings.on_url = on_url;
    settings.on_url_complete = on_url_complete;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_header_value_complete = on_header_value_complete;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;

    settings_init = true;
}

int parse_http_request(
    const char* data,
    size_t len,
    ParsedHttpRequest* out
) {
    ensure_settings();

    *out = {};

    static thread_local llhttp_t parser;
    static thread_local bool initialized = false;

    if (!initialized) {
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        initialized = true;
    } else {
        llhttp_reset(&parser);
    }

    ParserContext ctx{};
    ctx.out = out;

    parser.data = &ctx;

    llhttp_errno_t err = llhttp_execute(&parser, data, len);

    if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) {
        const char* pos = llhttp_get_error_pos(&parser);
        size_t header_end = pos ? static_cast<size_t>(pos - data) : len;

        if (out->no_body) {
            // F_SKIPBODY was set: llhttp fired message_complete right after
            // headers. pos points to the start of any body bytes the client
            // sent. We must advance total_consumed past them so the TCP
            // buffer is aligned for the next request on keep-alive connections.
            //
            // body_skip_len == UINT64_MAX means no Content-Length declared
            // (e.g. bare GET with no body). header_end is the full consumed size.
            const uint64_t cl = out->body_skip_len;
            if (cl != UINT64_MAX && cl > 0) {
                // Client declared a body. Check if all bytes are in the buffer.
                // Both the headers AND the body must be fully received before
                // we can complete this request (required for stream alignment).
                size_t total_needed = header_end + static_cast<size_t>(cl);
                if (len < total_needed) {
                    // Body incomplete — wait for remaining bytes.
                    return 0;
                }
                out->total_consumed = total_needed;
            } else {
                // No Content-Length (or 0): no body bytes follow the headers.
                out->total_consumed = header_end;
            }
        } else {
            out->total_consumed = header_end;

            // Chunked body reassembly for POST/PUT/PATCH etc.
            if (out->chunked &&
                out->chunked_body_ptr &&
                !out->chunked_body_ptr->empty()) {

                out->body = StringView(
                    out->chunked_body_ptr->data(),
                    out->chunked_body_ptr->size()
                );
            }
        }

        return 1;
    }

    if (err == HPE_OK) {
        return 0;
    }

    return -1;
}