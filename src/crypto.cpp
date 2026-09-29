// sha-1 (fips 180-4) and base64, no dependencies
#include "crypto.h"
#include <cstdint>
#include <cstring>

static uint32_t rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

std::string sha1_raw(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    uint64_t bit_len = (uint64_t)data.size() * 8;
    // padded message: data + 0x80 + zeros + 64-bit length
    size_t new_len = data.size() + 1;
    while (new_len % 64 != 56) new_len++;
    new_len += 8;
    std::string msg(new_len, 0);
    memcpy(&msg[0], data.data(), data.size());
    msg[data.size()] = (char)0x80;
    for (int i = 0; i < 8; i++)
        msg[new_len - 1 - i] = (char)(bit_len >> (i * 8));

    for (size_t off = 0; off < new_len; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)(unsigned char)msg[off + i * 4] << 24) |
                   ((uint32_t)(unsigned char)msg[off + i * 4 + 1] << 16) |
                   ((uint32_t)(unsigned char)msg[off + i * 4 + 2] << 8) |
                   (uint32_t)(unsigned char)msg[off + i * 4 + 3];
        }
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out(20, 0);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (char)(h[i] >> 24);
        out[i * 4 + 1] = (char)(h[i] >> 16);
        out[i * 4 + 2] = (char)(h[i] >> 8);
        out[i * 4 + 3] = (char)h[i];
    }
    return out;
}

std::string sha1_hex(const std::string& data) {
    static const char* hexd = "0123456789abcdef";
    std::string raw = sha1_raw(data);
    std::string out;
    for (unsigned char c : raw) {
        out += hexd[c >> 4];
        out += hexd[c & 0xf];
    }
    return out;
}

std::string base64_encode(const std::string& data) {
    static const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = (uint32_t)(unsigned char)data[i] << 16;
        if (i + 1 < data.size()) n |= (uint32_t)(unsigned char)data[i + 1] << 8;
        if (i + 2 < data.size()) n |= (uint32_t)(unsigned char)data[i + 2];
        out += alpha[(n >> 18) & 63];
        out += alpha[(n >> 12) & 63];
        out += i + 1 < data.size() ? alpha[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alpha[n & 63] : '=';
    }
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(const std::string& in, std::string& out) {
    out.clear();
    if (in.size() % 4 != 0) return false;
    for (size_t i = 0; i < in.size(); i += 4) {
        int vals[4];
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=') {
                vals[j] = 0;
                pad++;
            } else {
                vals[j] = b64_val(c);
                if (vals[j] < 0) return false;
            }
        }
        if (pad > 2) return false;
        uint32_t n = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6) |
                     vals[3];
        out += (char)(n >> 16);
        if (pad < 2) out += (char)((n >> 8) & 0xff);
        if (pad < 1) out += (char)(n & 0xff);
    }
    return true;
}
