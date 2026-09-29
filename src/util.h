#pragma once
#include <string>
#include <vector>

// small string helpers shared across servex modules
std::string trim(const std::string& s);
std::string lower(std::string s);
std::string upper(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char delim);
std::vector<std::string> split_ws(const std::string& s);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
// strict decimal parse, false on garbage or overflow
bool parse_long(const std::string& s, long& out);
// strict hex parse, false on garbage
bool parse_hex(const std::string& s, long& out);
std::string to_hex(long v);
// strip scheme://authority from an absolute request uri, leaving path?query
std::string strip_absolute_uri(const std::string& target);
// "a, b ,c" -> {"a","b","c"} with each token trimmed and lowered
std::vector<std::string> split_tokens(const std::string& s);
