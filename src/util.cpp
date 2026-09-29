// shared string helpers
#include "util.h"
#include <cctype>
#include <climits>
#include <sstream>

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = tolower((unsigned char)c);
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = toupper((unsigned char)c);
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(const std::string& a, const std::string& b) {
    return lower(a) == lower(b);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool parse_long(const std::string& s, long& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        i = 1;
    }
    if (i >= t.size()) return false;
    long v = 0;
    for (; i < t.size(); i++) {
        if (!isdigit((unsigned char)t[i])) return false;
        int d = t[i] - '0';
        if (v > (LONG_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    out = neg ? -v : v;
    return true;
}

bool parse_hex(const std::string& s, long& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    long v = 0;
    for (char c : t) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (v > (LONG_MAX - d) / 16) return false;
        v = v * 16 + d;
    }
    out = v;
    return true;
}

std::string to_hex(long v) {
    if (v == 0) return "0";
    static const char* digits = "0123456789abcdef";
    std::string out;
    unsigned long u = (unsigned long)v;
    while (u > 0) {
        out = digits[u % 16] + out;
        u /= 16;
    }
    return out;
}

std::string strip_absolute_uri(const std::string& target) {
    std::string t = target;
    size_t scheme = t.find("://");
    if (scheme != std::string::npos) {
        size_t slash = t.find('/', scheme + 3);
        if (slash == std::string::npos) return "/";
        t = t.substr(slash);
    }
    return t.empty() ? "/" : t;
}

std::vector<std::string> split_tokens(const std::string& s) {
    std::vector<std::string> out;
    for (auto& part : split(s, ',')) {
        std::string t = lower(trim(part));
        if (!t.empty()) out.push_back(t);
    }
    return out;
}
