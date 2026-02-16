#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ── WebSocket frame parser/builder (RFC 6455) ───────────────────────────────
// Parses client→server frames (masked) and builds server→client frames (unmasked).

enum WsOpcode : uint8_t {
    WS_CONTINUATION = 0x0,
    WS_TEXT         = 0x1,
    WS_BINARY       = 0x2,
    // 0x3-0x7 reserved for non-control frames
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xA,
    // 0xB-0xF reserved for control frames
};

struct WsFrame {
    bool fin;
    WsOpcode opcode;
    bool masked;
    uint8_t rsv;             // RSV1-3 bits (for extension negotiation)
    uint64_t payload_len;
    uint8_t mask_key[4];
    const uint8_t* payload;  // points into input buffer (after unmasking)
    size_t header_len;       // total frame header size consumed
};

// Parse one WebSocket frame from buffer.
// Returns:  >0 = total bytes consumed (header + payload)
//            0 = need more data
//           -1 = protocol error (generic)
//           -2 = unmasked client frame (RFC 6455 §5.1 violation)
//           -3 = reserved opcode (RFC 6455 §5.2)
//           -4 = reserved RSV bits set without extension (RFC 6455 §5.2)
int ws_parse_frame(const uint8_t* data, size_t len, WsFrame* out,
                   bool require_mask = true, bool allow_rsv = false);

// Unmask payload in-place (client→server frames are always masked)
void ws_unmask(uint8_t* payload, size_t len, const uint8_t mask[4]);

// Build a server→client frame (unmasked)
// Returns the frame bytes (header + payload)
std::vector<uint8_t> ws_build_frame(WsOpcode opcode, const uint8_t* payload, size_t len, bool fin = true);

// Build WebSocket upgrade response (101 Switching Protocols)
// sec_key: value of Sec-WebSocket-Key header
// Returns complete HTTP response bytes
std::vector<char> ws_build_upgrade_response(const char* sec_key, size_t key_len);

// Build a close frame with optional status code
std::vector<uint8_t> ws_build_close_frame(uint16_t status_code = 1000);

// ── High-performance helpers (write directly into caller buffer) ────────────

// Compute frame header size for a given payload length (2, 4, or 10 bytes)
inline size_t ws_frame_header_size(size_t payload_len) {
    if (payload_len < 126) return 2;
    if (payload_len <= 0xFFFF) return 4;
    return 10;
}

// Write frame header into buf (must have at least ws_frame_header_size() bytes).
// Returns number of bytes written.
size_t ws_write_frame_header(uint8_t* buf, WsOpcode opcode, size_t payload_len, bool fin = true);

// ── GIL-released echo helper (used by ws_echo_direct) ──────────────────────

struct EchoResult {
    size_t total_consumed = 0;
    bool has_close = false;
    size_t close_payload_offset = 0;  // offset into input buffer
    size_t close_payload_len = 0;
};

// Pure C++ echo: parse frames, unmask in-place, build echo response.
// No Python API calls — safe to call with GIL released.
void ws_echo_frames_nogil(
    uint8_t* data, size_t data_len,
    std::vector<uint8_t>& out, EchoResult& result);

// ── GIL-released parse helper (used by ws_handle_direct/json_direct) ─────

struct ParsedFrameRef {
    uint8_t opcode;
    size_t payload_offset;  // offset into input buffer (already unmasked in-place)
    size_t payload_len;
    bool is_assembled;      // true if this frame was reassembled from fragments
};

// Assembled message from continuation frames (owns its own buffer)
struct AssembledMessage {
    uint8_t opcode;                 // original opcode from first fragment
    std::vector<uint8_t> payload;   // reassembled payload data
};

struct ParseResult {
    std::vector<ParsedFrameRef> frames;
    std::vector<AssembledMessage> assembled;  // completed multi-fragment messages
    std::vector<uint8_t> pong_buf;
    size_t total_consumed = 0;
    bool pong_received = false;     // true when any PONG frame was seen
    int protocol_error = 0;         // non-zero if protocol error detected
};

// Fragment assembler — persists across data_received() calls per connection
struct WsFragmentAssembler {
    bool in_progress = false;
    uint8_t original_opcode = 0;    // opcode from first fragment (TEXT or BINARY)
    std::vector<uint8_t> accumulated;
    size_t max_message_size = 64 * 1024 * 1024;  // 64MB default

    // Feed a frame into the assembler.
    // Returns: 0=fragment stored (incomplete), 1=complete message ready, -1=protocol error
    int feed(const WsFrame& frame, const uint8_t* unmasked_payload);

    void reset() {
        in_progress = false;
        original_opcode = 0;
        accumulated.clear();
    }
};

// Pure C++ frame parser: parse + unmask all complete frames.
// No Python API calls — safe to call with GIL released.
// If assembler is non-null, handles continuation frame reassembly.
void ws_parse_frames_nogil(uint8_t* data, size_t data_len, ParseResult& result,
                           WsFragmentAssembler* assembler = nullptr);

// ── Per-message compression (RFC 7692 permessage-deflate) ────────────────

struct WsDeflateContext {
    bool enabled = false;
    bool server_no_context_takeover = false;
    bool client_no_context_takeover = false;
    int server_max_window_bits = 15;
    int client_max_window_bits = 15;
    size_t compress_threshold = 128;  // only compress messages > this size

    void* inflate_ctx = nullptr;  // z_stream* (opaque to avoid zlib include in header)
    void* deflate_ctx = nullptr;  // z_stream*

    bool init();
    void destroy();

    // Decompress incoming frame payload. Returns false on error.
    bool decompress(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out);
    // Compress outgoing frame payload. Returns false on error.
    bool compress(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out);
};

// Build upgrade response with optional extension negotiation
// If client requests permessage-deflate and deflate_out is non-null,
// negotiation result is stored in *deflate_out.
std::vector<char> ws_build_upgrade_response_ext(
    const char* sec_key, size_t key_len,
    const char* extensions, size_t ext_len,
    const char* subprotocol, size_t sub_len,
    WsDeflateContext* deflate_out);
