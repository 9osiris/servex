#pragma once
#include <string>
#include <vector>

// single resolved byte range, start/end inclusive
struct ByteRange {
    long start;
    long end;
};

// parse a Range header value and resolve it against size.
// returns false when nothing is satisfiable (caller sends 416).
bool parse_ranges(const std::string& value, long size,
                  std::vector<ByteRange>& out);
// if-range validator check against the current etag and mtime
bool if_range_matches(const std::string& value, const std::string& etag,
                      long mtime);
// "bytes 0-99/1234" for a single range
std::string content_range_value(const ByteRange& r, long size);
