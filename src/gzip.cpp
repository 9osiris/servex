// stdlib-only gzip: crc32 plus a fixed-huffman deflate encoder
// (rfc 1951) wrapped in the gzip container (rfc 1952).
#include "gzip.h"
#include <cstdint>
#include <vector>

namespace {

// crc32, ieee polynomial, table built once
uint32_t crc32_table[256];
bool crc32_ready = false;

void init_crc32() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

uint32_t crc32(const std::string& data) {
    if (!crc32_ready) init_crc32();
    uint32_t c = 0xffffffffu;
    for (unsigned char ch : data)
        c = crc32_table[(c ^ ch) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// bit writer, lsb-first packing like the wire wants
struct BitWriter {
    std::string out;
    unsigned buf = 0;
    int nbits = 0;

    void write_bit(int b) {
        buf |= (unsigned)(b & 1) << nbits;
        if (++nbits == 8) {
            out.push_back((char)buf);
            buf = 0;
            nbits = 0;
        }
    }

    // code is given msb-first, emit in stream order
    void write_code(unsigned code, int n) {
        for (int i = n - 1; i >= 0; i--)
            write_bit((code >> i) & 1);
    }

    // extra bits pack lsb-first, unlike the huffman codes above
    void write_extra(unsigned v, int n) {
        for (int i = 0; i < n; i++)
            write_bit((v >> i) & 1);
    }

    void flush() {
        if (nbits > 0) {
            out.push_back((char)buf);
            buf = 0;
            nbits = 0;
        }
    }
};

// fixed huffman literal/length code for symbol s (0..287)
void write_litlen(BitWriter& w, int s) {
    if (s < 144) w.write_code(0x30 + (unsigned)s, 8);
    else if (s < 256) w.write_code(0x190 + (unsigned)(s - 144), 9);
    else if (s < 280) w.write_code((unsigned)(s - 256), 7);
    else w.write_code(0xc0 + (unsigned)(s - 280), 8);
}

// fixed huffman distance code (0..29), plain 5 bit value
void write_dist(BitWriter& w, int d) {
    w.write_code((unsigned)d, 5);
}

const int LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const int LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const int DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
const int DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// emit one length/distance pair
void write_match(BitWriter& w, int len, int dist) {
    int li = 0;
    while (li < 28 && len >= LEN_BASE[li + 1]) li++;
    write_litlen(w, 257 + li);
    if (LEN_EXTRA[li] > 0)
        w.write_extra((unsigned)(len - LEN_BASE[li]), LEN_EXTRA[li]);
    int di = 0;
    while (di < 29 && dist >= DIST_BASE[di + 1]) di++;
    write_dist(w, di);
    if (DIST_EXTRA[di] > 0)
        w.write_extra((unsigned)(dist - DIST_BASE[di]), DIST_EXTRA[di]);
}

const size_t BLOCK_MAX = 32768; // deflate window, keeps matches in range

// one fixed-huffman block over in[base, base+len)
void deflate_block(BitWriter& w, const std::string& in, size_t base,
                   size_t len, bool last) {
    w.write_bit(last ? 1 : 0); // bfinal
    w.write_bit(1);            // btype 01: fixed huffman
    w.write_bit(0);

    // last position seen for each 3-byte hash, reset per block
    const int HBITS = 15;
    std::vector<int> head(1 << HBITS, -1);
    auto hash3 = [&](size_t p) -> int {
        unsigned h = (unsigned char)in[base + p];
        h = h * 31 + (unsigned char)in[base + p + 1];
        h = h * 31 + (unsigned char)in[base + p + 2];
        return (int)(h & ((1 << HBITS) - 1));
    };

    size_t pos = 0;
    size_t end = len;
    while (pos < end) {
        int best_len = 0, best_dist = 0;
        if (pos + 3 <= end) {
            int h = hash3(pos);
            int prev = head[h];
            if (prev >= 0) {
                size_t dist = pos - (size_t)prev;
                if (dist >= 1 && dist <= 32768) {
                    size_t ml = 0;
                    while (ml < 258 && pos + ml < end &&
                           in[base + prev + ml] == in[base + pos + ml])
                        ml++;
                    if (ml >= 3) {
                        best_len = (int)ml;
                        best_dist = (int)dist;
                    }
                }
            }
            head[h] = (int)pos;
        }
        if (best_len >= 3) {
            write_match(w, best_len, best_dist);
            // keep the hash table roughly current across the match
            for (int k = 1; k < best_len && pos + (size_t)k + 3 <= end; k++)
                head[hash3(pos + k)] = (int)(pos + k);
            pos += best_len;
        } else {
            write_litlen(w, (unsigned char)in[base + pos]);
            pos++;
        }
    }
    write_litlen(w, 256); // end of block
}

std::string deflate_fixed(const std::string& in) {
    BitWriter w;
    size_t n = in.size();
    if (n == 0) {
        w.write_bit(1); // bfinal
        w.write_bit(1); // fixed huffman
        w.write_bit(0);
        write_litlen(w, 256);
        w.flush();
        return w.out;
    }
    size_t off = 0;
    while (off < n) {
        size_t len = n - off > BLOCK_MAX ? BLOCK_MAX : n - off;
        deflate_block(w, in, off, len, off + len == n);
        off += len;
    }
    w.flush();
    return w.out;
}

void put_u32le(std::string& out, uint32_t v) {
    out.push_back((char)(v & 0xff));
    out.push_back((char)((v >> 8) & 0xff));
    out.push_back((char)((v >> 16) & 0xff));
    out.push_back((char)((v >> 24) & 0xff));
}

} // namespace

std::string gzip_compress(const std::string& data) {
    std::string out;
    out.push_back('\x1f');
    out.push_back('\x8b');
    out.push_back(8); // deflate
    out.push_back(0); // no flags
    put_u32le(out, 0); // mtime: none
    out.push_back(0); // xfl: none
    out.push_back(3); // os: unix
    out += deflate_fixed(data);
    put_u32le(out, crc32(data));
    put_u32le(out, (uint32_t)data.size());
    return out;
}
