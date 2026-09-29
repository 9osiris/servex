#pragma once
#include <map>
#include <string>
#include <vector>

// etag generation and conditional request evaluation

// strong etag for a file: "<hex(mtime)>-<hex(size)>"
std::string make_etag(long size, long mtime);
std::string make_weak_etag(long size, long mtime);

// format a time_t as an IMF-fixdate http date
std::string http_date(long t);
// parse an http date (imf-fixdate, rfc850, or asctime), false on garbage
bool parse_http_date(const std::string& s, long& out);

// quote-aware split of an etag list header on commas
std::vector<std::string> split_etag_list(const std::string& s);
// weak comparison: true if header (an if-none-match value) matches etag
bool etag_weak_match(const std::string& header, const std::string& etag);
// strong comparison: true if header (an if-match value) matches etag
bool etag_strong_match(const std::string& header, const std::string& etag);

// evaluate rfc 9110 preconditions for a static file.
// returns 0 to proceed, or 304 / 412.
int eval_preconditions(const std::map<std::string, std::string>& headers,
                       const std::string& method, const std::string& etag,
                       long mtime);
