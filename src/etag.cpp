// etag generation, http dates, and conditional request evaluation
#include "etag.h"
#include "util.h"
#include <cstdio>
#include <ctime>

std::string make_etag(long size, long mtime) {
    return "\"" + to_hex(mtime) + "-" + to_hex(size) + "\"";
}

std::string make_weak_etag(long size, long mtime) {
    return "W/\"" + to_hex(mtime) + "-" + to_hex(size) + "\"";
}

std::string http_date(long t) {
    char buf[64];
    std::time_t tt = (std::time_t)t;
    std::tm tm;
    gmtime_r(&tt, &tm);
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return std::string(buf);
}

static int month_num(const std::string& m) {
    static const char* names[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                  "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string l = lower(m);
    for (int i = 0; i < 12; i++)
        if (l == names[i]) return i + 1;
    return 0;
}

// days since 1970-01-01 for a civil date (no libc timezone tricks)
static long days_from_civil(long y, long m, long d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool civil_to_time(long y, long mo, long d, int hh, int mm, int ss,
                          long& out) {
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60)
        return false;
    out = days_from_civil(y, mo, d) * 86400L + hh * 3600L + mm * 60L + ss;
    return true;
}

bool parse_http_date(const std::string& s, long& out) {
    std::string t = trim(s);
    char mon[16] = {0};
    int d, y, hh, mm, ss;
    // imf-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
    if (std::sscanf(t.c_str(), "%*3s, %d %15[A-Za-z] %d %d:%d:%d GMT", &d, mon,
                    &y, &hh, &mm, &ss) == 6) {
        int mo = month_num(mon);
        if (mo && civil_to_time(y, mo, d, hh, mm, ss, out)) return true;
        return false;
    }
    // rfc850: Sunday, 06-Nov-94 08:49:37 GMT
    if (std::sscanf(t.c_str(), "%*[A-Za-z], %d-%15[A-Za-z]-%d %d:%d:%d GMT", &d,
                    mon, &y, &hh, &mm, &ss) == 6) {
        int mo = month_num(mon);
        if (y < 100) y += (y >= 70 ? 1900 : 2000);
        if (mo && civil_to_time(y, mo, d, hh, mm, ss, out)) return true;
        return false;
    }
    // asctime: Sun Nov  6 08:49:37 1994
    char day[8] = {0};
    if (std::sscanf(t.c_str(), "%7[A-Za-z] %15[A-Za-z] %d %d:%d:%d %d", day, mon,
                    &d, &hh, &mm, &ss, &y) == 7) {
        int mo = month_num(mon);
        if (mo && civil_to_time(y, mo, d, hh, mm, ss, out)) return true;
        return false;
    }
    return false;
}

std::vector<std::string> split_etag_list(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (char c : s) {
        if (c == '"') in_quote = !in_quote;
        if (c == ',' && !in_quote) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

// strip a leading W/ and surrounding quotes, returning the opaque tag
static std::string opaque_tag(const std::string& e, bool& weak) {
    std::string t = trim(e);
    weak = false;
    if (t.size() > 2 && (t[0] == 'W' || t[0] == 'w') && t[1] == '/') {
        weak = true;
        t = trim(t.substr(2));
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = t.substr(1, t.size() - 2);
    return t;
}

bool etag_weak_match(const std::string& header, const std::string& etag) {
    std::string h = trim(header);
    if (h == "*") return true;
    bool ew = false;
    std::string etag_opaque = opaque_tag(etag, ew);
    for (auto& item : split_etag_list(h)) {
        bool iw = false;
        if (opaque_tag(item, iw) == etag_opaque) return true;
    }
    return false;
}

bool etag_strong_match(const std::string& header, const std::string& etag) {
    std::string h = trim(header);
    if (h == "*") return true;
    bool ew = false;
    std::string etag_opaque = opaque_tag(etag, ew);
    if (ew) return false; // current rep must be strong too
    for (auto& item : split_etag_list(h)) {
        bool iw = false;
        std::string io = opaque_tag(item, iw);
        if (!iw && io == etag_opaque) return true;
    }
    return false;
}

int eval_preconditions(const std::map<std::string, std::string>& headers,
                       const std::string& method, const std::string& etag,
                       long mtime) {
    bool is_get = (method == "GET" || method == "HEAD");
    auto find = [&](const char* n) {
        auto it = headers.find(n);
        return it == headers.end() ? nullptr : &it->second;
    };
    const std::string* if_match = find("if-match");
    const std::string* if_unmod = find("if-unmodified-since");
    const std::string* if_none = find("if-none-match");
    const std::string* if_mod = find("if-modified-since");

    // step 1: if-match / if-unmodified-since
    if (if_match) {
        if (!etag_strong_match(*if_match, etag)) return 412;
    } else if (if_unmod) {
        long d;
        if (parse_http_date(*if_unmod, d) && mtime > d) return 412;
    }
    // step 2: if-none-match / if-modified-since
    if (if_none) {
        if (etag_weak_match(*if_none, etag)) return is_get ? 304 : 412;
    } else if (if_mod && is_get) {
        long d;
        if (parse_http_date(*if_mod, d) && mtime <= d) return 304;
    }
    return 0;
}
