// byte range parsing per rfc 9110 section 14
#include "range.h"
#include "etag.h"
#include "util.h"

bool parse_ranges(const std::string& value, long size,
                  std::vector<ByteRange>& out) {
    out.clear();
    std::string v = trim(value);
    size_t eq = v.find('=');
    if (eq == std::string::npos) return false;
    if (lower(trim(v.substr(0, eq))) != "bytes") return false;
    std::string specs = v.substr(eq + 1);
    for (auto& spec : split(specs, ',')) {
        std::string s = trim(spec);
        size_t dash = s.find('-');
        if (dash == std::string::npos) continue;
        std::string left = trim(s.substr(0, dash));
        std::string right = trim(s.substr(dash + 1));
        long a = 0, b = 0;
        ByteRange r{0, -1};
        if (left.empty()) {
            // suffix range: the last N bytes
            if (!parse_long(right, b) || b <= 0) continue;
            if (b >= size) {
                r.start = 0;
            } else {
                r.start = size - b;
            }
            r.end = size - 1;
        } else {
            if (!parse_long(left, a) || a < 0) continue;
            if (!right.empty()) {
                if (!parse_long(right, b) || b < 0) continue;
                if (a > b) continue; // backwards range, ignore it
                r.start = a;
                r.end = b >= size ? size - 1 : b;
            } else {
                r.start = a;
                r.end = size - 1;
            }
        }
        if (r.start >= size) continue; // starts past the end
        if (r.end < r.start) continue;
        out.push_back(r);
    }
    // merge overlapping/adjacent ranges to keep output small
    for (size_t i = 0; i < out.size(); i++) {
        for (size_t j = i + 1; j < out.size();) {
            if (out[j].start <= out[i].end + 1 &&
                out[i].start <= out[j].end + 1) {
                if (out[j].start < out[i].start) out[i].start = out[j].start;
                if (out[j].end > out[i].end) out[i].end = out[j].end;
                out.erase(out.begin() + j);
            } else {
                j++;
            }
        }
    }
    return !out.empty();
}

bool if_range_matches(const std::string& value, const std::string& etag,
                      long mtime) {
    std::string v = trim(value);
    if (v.empty()) return true;
    if (v[0] == '"' || (v.size() > 1 && (v[0] == 'W' || v[0] == 'w'))) {
        return etag_strong_match(v, etag);
    }
    long d;
    if (!parse_http_date(v, d)) return false;
    return mtime <= d;
}

std::string content_range_value(const ByteRange& r, long size) {
    return "bytes " + std::to_string(r.start) + "-" + std::to_string(r.end) +
           "/" + std::to_string(size);
}
