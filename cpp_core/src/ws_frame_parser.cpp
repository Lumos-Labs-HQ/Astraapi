#include "ws_frame_parser.hpp"
#include <cstring>
#include <string>

// ── Minimal SHA-1 (RFC 3174) — only for WebSocket handshake ─────────────────
// Security note: SHA-1 is used here ONLY for RFC 6455 protocol compliance,
// NOT for cryptographic purposes.

namespace {

struct SHA1 {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];

    SHA1() {
        state[0] = 0x67452301;
        state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE;
        state[3] = 0x10325476;
        state[4] = 0xC3D2E1F0;
        count = 0;
        memset(buffer, 0, 64);
    }

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void transform(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
                    ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                 k = 0xCA62C1D6; }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        size_t idx = (size_t)(count % 64);
        count += len;

        for (size_t i = 0; i < len; i++) {
            buffer[idx++] = data[i];
            if (idx == 64) {
                transform(buffer);
                idx = 0;
            }
        }
    }

    void finalize(uint8_t digest[20]) {
        uint64_t bits = count * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while ((count % 64) != 56) update(&pad, 1);

        uint8_t len_be[8];
        for (int i = 7; i >= 0; i--) {
            len_be[i] = (uint8_t)(bits & 0xFF);
            bits >>= 8;
        }
        update(len_be, 8);

        for (int i = 0; i < 5; i++) {
            digest[i*4]     = (uint8_t)(state[i] >> 24);
            digest[i*4 + 1] = (uint8_t)(state[i] >> 16);
            digest[i*4 + 2] = (uint8_t)(state[i] >> 8);
            digest[i*4 + 3] = (uint8_t)(state[i]);
        }
    }
};

// ── Base64 encode ───────────────────────────────────────────────────────────

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i+1]) << 8;
        if (i + 2 < len) n |= (uint32_t)data[i+2];

        result.push_back(b64_table[(n >> 18) & 0x3F]);
        result.push_back(b64_table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? b64_table[n & 0x3F] : '=');
    }
    return result;
}

} // anonymous namespace

// ── Frame parser ────────────────────────────────────────────────────────────

int ws_parse_frame(const uint8_t* data, size_t len, WsFrame* out) {
    if (len < 2) return 0;  // need more data

    out->fin = (data[0] & 0x80) != 0;
    out->opcode = (WsOpcode)(data[0] & 0x0F);
    out->masked = (data[1] & 0x80) != 0;

    uint64_t payload_len = data[1] & 0x7F;
    size_t pos = 2;

    if (payload_len == 126) {
        if (len < 4) return 0;
        payload_len = ((uint64_t)data[2] << 8) | (uint64_t)data[3];
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) return 0;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | (uint64_t)data[2 + i];
        }
        pos = 10;
    }

    // Limit payload to 64MB to prevent abuse
    if (payload_len > 64 * 1024 * 1024) return -1;

    if (out->masked) {
        if (len < pos + 4) return 0;
        memcpy(out->mask_key, data + pos, 4);
        pos += 4;
    } else {
        memset(out->mask_key, 0, 4);
    }

    if (len < pos + payload_len) return 0;  // need more data

    out->payload_len = payload_len;
    out->payload = data + pos;
    out->header_len = pos;

    return (int)(pos + payload_len);
}

void ws_unmask(uint8_t* payload, size_t len, const uint8_t mask[4]) {
    // Unmask 8 bytes at a time for speed
    size_t i = 0;
    if (len >= 8) {
        uint32_t mask32;
        memcpy(&mask32, mask, 4);
        uint64_t mask64 = ((uint64_t)mask32 << 32) | (uint64_t)mask32;
        for (; i + 7 < len; i += 8) {
            uint64_t chunk;
            memcpy(&chunk, payload + i, 8);
            chunk ^= mask64;
            memcpy(payload + i, &chunk, 8);
        }
    }
    for (; i < len; i++) {
        payload[i] ^= mask[i & 3];
    }
}

// ── Frame builder ───────────────────────────────────────────────────────────

std::vector<uint8_t> ws_build_frame(WsOpcode opcode, const uint8_t* payload, size_t len, bool fin) {
    std::vector<uint8_t> frame;
    frame.reserve(10 + len);  // max header + payload

    // Byte 0: FIN + opcode
    frame.push_back((fin ? 0x80 : 0x00) | (uint8_t)opcode);

    // Byte 1+: payload length (server→client: no mask)
    if (len < 126) {
        frame.push_back((uint8_t)len);
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
        }
    }

    // Payload (unmasked for server→client)
    frame.insert(frame.end(), payload, payload + len);
    return frame;
}

std::vector<uint8_t> ws_build_close_frame(uint16_t status_code) {
    uint8_t payload[2] = { (uint8_t)(status_code >> 8), (uint8_t)(status_code & 0xFF) };
    return ws_build_frame(WS_CLOSE, payload, 2);
}

// ── WebSocket upgrade handshake (RFC 6455 §4.2.2) ──────────────────────────

static const char WS_MAGIC_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::vector<char> ws_build_upgrade_response(const char* sec_key, size_t key_len) {
    // Compute Sec-WebSocket-Accept = base64(SHA1(key + magic_guid))
    SHA1 sha;
    sha.update((const uint8_t*)sec_key, key_len);
    sha.update((const uint8_t*)WS_MAGIC_GUID, sizeof(WS_MAGIC_GUID) - 1);
    uint8_t digest[20];
    sha.finalize(digest);
    std::string accept = base64_encode(digest, 20);

    // Build HTTP 101 response
    std::string resp;
    resp.reserve(256);
    resp += "HTTP/1.1 101 Switching Protocols\r\n";
    resp += "Upgrade: websocket\r\n";
    resp += "Connection: Upgrade\r\n";
    resp += "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n\r\n";

    return std::vector<char>(resp.begin(), resp.end());
}
