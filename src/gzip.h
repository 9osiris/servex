#pragma once
#include <string>

// stdlib-only gzip compressor: fixed-huffman deflate (rfc 1951)
// wrapped in the gzip container (rfc 1952). no tables beyond a
// small runtime crc32 table, no dependencies.
std::string gzip_compress(const std::string& data);

// knobs for the static file server, from the config
struct GzipOpts {
    bool enabled = false;
    long min_size = 1024;
};
