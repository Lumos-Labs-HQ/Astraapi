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
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xA,
};

struct WsFrame {
    bool fin;
    WsOpcode opcode;
    bool masked;
    uint64_t payload_len;
    uint8_t mask_key[4];
    const uint8_t* payload;  // points into input buffer (after unmasking)
    size_t header_len;       // total frame header size consumed
};

// Parse one WebSocket frame from buffer.
// Returns:  >0 = total bytes consumed (header + payload)
//            0 = need more data
//           -1 = protocol error
int ws_parse_frame(const uint8_t* data, size_t len, WsFrame* out);

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
