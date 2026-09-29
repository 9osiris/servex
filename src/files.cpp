// static file serving with mime detection, conditional requests,
// and byte ranges
#include "files.h"
#include "etag.h"
#include "range.h"
#include "util.h"
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>
#include <vector>

static const long MAX_MULTIPART_BYTES = 8 * 1024 * 1024;

std::string mime_type(const std::string& filename) {
    size_t dot = filename.rfind('.');
    std::string ext = dot == std::string::npos ? "" : filename.substr(dot + 1);
    for (char& c : ext) c = tolower((unsigned char)c);
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js") return "text/javascript";
    if (ext == "mjs") return "text/javascript";
    if (ext == "json") return "application/json";
    if (ext == "txt" || ext == "text" || ext == "md") return "text/plain";
    if (ext == "csv") return "text/csv";
    if (ext == "xml") return "application/xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "webp") return "image/webp";
    if (ext == "avif") return "image/avif";
    if (ext == "bmp") return "image/bmp";
    if (ext == "pdf") return "application/pdf";
    if (ext == "zip") return "application/zip";
    if (ext == "gz" || ext == "tgz") return "application/gzip";
    if (ext == "tar") return "application/x-tar";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "ogg" || ext == "oga") return "audio/ogg";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf") return "font/ttf";
    if (ext == "otf") return "font/otf";
    return "application/octet-stream";
}

// join docroot + request path, resolving . and .. ; "" on escape attempt
static std::string join_safe(const std::string& docroot,
                             const std::string& req_path) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= req_path.size(); i++) {
        char c = i < req_path.size() ? req_path[i] : '/';
        if (c == '/') {
            if (cur == "..") {
                if (parts.empty()) return "";
                parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string full = docroot;
    for (auto& p : parts) full += "/" + p;
    return full;
}

static std::string html_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

static HttpResponse list_directory(const std::string& full,
                                   const std::string& req_path) {
    DIR* d = opendir(full.c_str());
    if (!d) return make_error(403, "cannot read directory");
    std::vector<std::string> names;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == ".") continue;
        names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    std::string title = "Index of " + req_path;
    std::string body = "<!doctype html>\n<html>\n<head><title>" +
        html_escape(title) + "</title></head>\n<body>\n<h1>" +
        html_escape(title) + "</h1>\n<hr>\n<ul>\n";
    for (auto& n : names) {
        struct stat st;
        bool is_dir = stat((full + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        std::string href = n + (is_dir ? "/" : "");
        body += "<li><a href=\"" + html_escape(href) + "\">" +
                html_escape(href) + "</a></li>\n";
    }
    body += "</ul>\n<hr>\n</body>\n</html>\n";
    HttpResponse r;
    r.headers["Content-Type"] = "text/html";
    r.body = body;
    return r;
}

static HttpResponse method_not_allowed() {
    HttpResponse r = make_error(405, "only GET and HEAD are served here");
    r.headers["Allow"] = "GET, HEAD, OPTIONS";
    return r;
}

static HttpResponse range_not_satisfiable(long size) {
    HttpResponse r = make_error(416, "no satisfiable byte range");
    r.headers["Content-Range"] = "bytes */" + std::to_string(size);
    return r;
}

// read [offset, offset+len) from a file
static std::string read_slice(const std::string& path, long offset, long len) {
    std::string out;
    if (len <= 0) return out;
    int f = open(path.c_str(), O_RDONLY);
    if (f < 0) return out;
    out.resize((size_t)len);
    long got = 0;
    while (got < len) {
        ssize_t r = pread(f, &out[got], (size_t)(len - got), offset + got);
        if (r <= 0) break;
        got += r;
    }
    out.resize((size_t)got);
    close(f);
    return out;
}

static std::string make_boundary() {
    static std::mt19937 rng((unsigned long)time(nullptr) ^
                            (unsigned long)getpid());
    std::string b = "servex";
    for (int i = 0; i < 16; i++) b += "0123456789abcdef"[rng() % 16];
    return b;
}

// build a multipart/byteranges body for the resolved ranges
static HttpResponse multipart_response(const std::string& full, long size,
                                       const std::vector<ByteRange>& ranges,
                                       const std::string& ctype) {
    long total = 0;
    for (auto& r : ranges) total += r.end - r.start + 1;
    if (total > MAX_MULTIPART_BYTES)
        return make_error(413, "multipart range body too large");
    std::string boundary = make_boundary();
    std::string body;
    for (auto& r : ranges) {
        body += "--" + boundary + "\r\n";
        body += "Content-Type: " + ctype + "\r\n";
        body += "Content-Range: " + content_range_value(r, size) + "\r\n\r\n";
        body += read_slice(full, r.start, r.end - r.start + 1);
        body += "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    HttpResponse res;
    res.status = 206;
    res.reason = reason_phrase(206);
    res.headers["Content-Type"] = "multipart/byteranges; boundary=" + boundary;
    res.body = body;
    return res;
}

HttpResponse serve_file_request(const std::string& docroot,
                                const HttpRequest& req, bool dir_listing) {
    if (req.method != "GET" && req.method != "HEAD")
        return method_not_allowed();
    std::string full = join_safe(docroot, req.path);
    if (full.empty())
        return make_error(403, "path escapes the document root");
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
        return make_error(404, "no such file: " + req.path);
    if (S_ISDIR(st.st_mode)) {
        if (req.path.empty() || req.path.back() != '/') {
            HttpResponse r;
            r.status = 301;
            r.reason = "Moved Permanently";
            r.headers["Location"] = req.path + "/";
            return r;
        }
        for (const char* idx : {"index.html", "index.htm"}) {
            std::string p = full + "/" + idx;
            struct stat ist;
            if (stat(p.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) {
                full = p;
                st = ist;
                break;
            }
        }
        if (S_ISDIR(st.st_mode)) {
            if (dir_listing) return list_directory(full, req.path);
            return make_error(403, "directory listing is disabled");
        }
    }
    if (!S_ISREG(st.st_mode))
        return make_error(403, "not a regular file");
    size_t slash = full.rfind('/');
    std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
    std::string ctype = mime_type(name);
    std::string etag = make_etag(st.st_size, st.st_mtime);
    std::string lastmod = http_date(st.st_mtime);

    int pre = eval_preconditions(req.headers, req.method, etag, st.st_mtime);
    if (pre == 412) return make_error(412, "precondition failed");
    if (pre == 304) {
        HttpResponse r;
        r.status = 304;
        r.reason = reason_phrase(304);
        r.headers["ETag"] = etag;
        r.headers["Last-Modified"] = lastmod;
        return r;
    }

    // byte ranges only apply to GET on a real file
    if (req.method == "GET") {
        auto rit = req.headers.find("range");
        if (rit != req.headers.end()) {
            std::vector<ByteRange> ranges;
            if (!parse_ranges(rit->second, st.st_size, ranges))
                return range_not_satisfiable(st.st_size);
            auto iit = req.headers.find("if-range");
            if (iit == req.headers.end() ||
                if_range_matches(iit->second, etag, st.st_mtime)) {
                if (ranges.size() == 1) {
                    HttpResponse r;
                    r.status = 206;
                    r.reason = reason_phrase(206);
                    r.headers["Content-Type"] = ctype;
                    r.headers["Content-Range"] =
                        content_range_value(ranges[0], st.st_size);
                    r.headers["ETag"] = etag;
                    r.headers["Last-Modified"] = lastmod;
                    r.headers["Accept-Ranges"] = "bytes";
                    r.is_file = true;
                    r.file_path = full;
                    r.file_offset = ranges[0].start;
                    r.file_size = ranges[0].end - ranges[0].start + 1;
                    return r;
                }
                HttpResponse r = multipart_response(full, st.st_size, ranges,
                                                    ctype);
                r.headers["ETag"] = etag;
                r.headers["Last-Modified"] = lastmod;
                r.headers["Accept-Ranges"] = "bytes";
                return r;
            }
            // if-range failed: fall through to the full 200 response
        }
    }

    HttpResponse r;
    r.headers["Content-Type"] = ctype;
    r.headers["ETag"] = etag;
    r.headers["Last-Modified"] = lastmod;
    r.headers["Accept-Ranges"] = "bytes";
    r.is_file = true;
    r.file_path = full;
    r.file_size = st.st_size;
    return r;
}
