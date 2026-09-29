// http/1.x request parsing and response writing
#include "http.h"
#include "util.h"
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

static const size_t MAX_HEADER_BYTES = 65536;
static const size_t MAX_LINE_BYTES = 16384;
static const long MAX_BODY_BYTES = 8 * 1024 * 1024;

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() &&
            isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            int v = std::stoi(s.substr(i + 1, 2), nullptr, 16);
            out += (char)v;
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Error";
    }
}

HttpResponse make_error(int status, const std::string& message) {
    HttpResponse r;
    r.status = status;
    r.reason = reason_phrase(status);
    r.headers["Content-Type"] = "text/html";
    r.body = "<html><body><h1>" + std::to_string(status) + " " + r.reason +
             "</h1><p>" + message + "</p></body></html>";
    return r;
}

HttpResponse make_options_response(const HttpRequest&) {
    HttpResponse r;
    r.status = 200;
    r.reason = "OK";
    r.headers["Allow"] = "GET, HEAD, POST, PUT, DELETE, OPTIONS";
    r.headers["Content-Length"] = "0";
    return r;
}

bool uses_chunked(const HttpRequest& req) {
    auto it = req.headers.find("transfer-encoding");
    if (it == req.headers.end()) return false;
    std::vector<std::string> toks = split_tokens(it->second);
    return !toks.empty() && toks.back() == "chunked";
}

// buffered reader over leftover pending bytes plus the socket
struct BodyReader {
    int fd;
    std::string buf; // unread bytes
    long* wire;      // optional byte counter
    bool dead = false;

    bool fill() {
        char tmp[4096];
        ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
        if (r <= 0) {
            dead = true;
            return false;
        }
        if (wire) *wire += r;
        buf.append(tmp, r);
        return true;
    }

    // read one crlf-terminated line, false on eof/error/oversize
    bool read_line(std::string& line) {
        while (true) {
            size_t p = buf.find("\r\n");
            if (p != std::string::npos) {
                line = buf.substr(0, p);
                buf.erase(0, p + 2);
                return true;
            }
            if (buf.size() > MAX_LINE_BYTES) return false;
            if (!fill()) return false;
        }
    }

    // read exactly n bytes, false on eof/error
    bool read_bytes(size_t n, std::string& out) {
        out.clear();
        out.reserve(n);
        while (out.size() < n) {
            if (buf.empty() && !fill()) return false;
            size_t take = buf.size();
            if (take > n - out.size()) take = n - out.size();
            out.append(buf, 0, take);
            buf.erase(0, take);
        }
        return true;
    }
};

// decode a chunked body per rfc 9112 section 7.1. leftovers stay in br.buf.
static bool decode_chunked(BodyReader& br, std::string& body, int& error_status) {
    body.clear();
    while (true) {
        std::string line;
        if (!br.read_line(line)) {
            error_status = 400;
            return false;
        }
        size_t semi = line.find(';'); // strip chunk extensions
        long chunk_len = 0;
        if (!parse_hex(semi == std::string::npos ? line : line.substr(0, semi),
                       chunk_len) ||
            chunk_len < 0) {
            error_status = 400;
            return false;
        }
        if (chunk_len == 0) {
            // consume trailers until the blank line
            while (br.read_line(line)) {
                if (line.empty()) break;
            }
            return !br.dead;
        }
        if ((long)body.size() + chunk_len > MAX_BODY_BYTES) {
            error_status = 413;
            return false;
        }
        std::string chunk;
        if (!br.read_bytes((size_t)chunk_len, chunk)) {
            error_status = 400;
            return false;
        }
        body += chunk;
        if (!br.read_line(line) || !line.empty()) {
            error_status = 400; // missing crlf after chunk data
            return false;
        }
    }
}

static long send_all(int fd, const char* p, size_t n) {
    long sent = 0;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return sent;
        p += r;
        n -= r;
        sent += r;
    }
    return sent;
}

// parse one request, keeping pipelined bytes in pending for next time
bool read_request(int fd, std::string& pending, HttpRequest& out,
                  long* wire_in) {
    out = HttpRequest();
    char buf[4096];
    size_t hlen = std::string::npos;
    while (hlen == std::string::npos) {
        size_t p = pending.find("\r\n\r\n");
        if (p != std::string::npos) {
            hlen = p + 4;
            break;
        }
        if (pending.size() > MAX_HEADER_BYTES) return false; // too big, drop it
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) return false;
        if (wire_in) *wire_in += r;
        pending.append(buf, r);
    }
    std::string head = pending.substr(0, hlen);
    std::string avail = pending.substr(hlen);
    pending.clear();

    std::istringstream hs(head);
    std::string line;
    if (!std::getline(hs, line)) return true; // empty, stays invalid
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream rl(line);
    std::string method, target, version;
    if (!(rl >> method >> target >> version)) return true; // malformed
    std::string extra;
    if (rl >> extra) return true; // junk after version
    target = strip_absolute_uri(target);
    if (method.empty() || target.empty()) return true;
    // asterisk-form is only valid for OPTIONS
    if (target != "*" && target[0] != '/') return true;
    if (version.compare(0, 5, "HTTP/") != 0) return true;

    out.method = method;
    out.target = target;
    out.version = version;
    size_t q = target.find('?');
    out.path = url_decode(q == std::string::npos ? target : target.substr(0, q));
    out.query = q == std::string::npos ? "" : target.substr(q + 1);

    bool saw_content_length = false;
    std::string first_content_length;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t c = line.find(':');
        if (c == std::string::npos) return true; // bad header line
        std::string name = lower(trim(line.substr(0, c)));
        std::string value = trim(line.substr(c + 1));
        if (name.empty()) return true;
        if (name == "content-length") {
            // conflicting duplicate content-length values are smuggling bait
            if (saw_content_length && value != first_content_length) return true;
            saw_content_length = true;
            first_content_length = value;
        }
        if (name == "transfer-encoding" && out.headers.count(name))
            value = out.headers[name] + ", " + value; // merge duplicates
        out.headers[name] = value;
    }

    bool te_chunked = uses_chunked(out);
    bool has_te = out.headers.count("transfer-encoding") > 0;
    if (has_te && !te_chunked) {
        out.error_status = 501; // we only speak chunked
        return true;
    }
    if (te_chunked && saw_content_length) return true; // both is a 400

    auto eit = out.headers.find("expect");
    if (eit != out.headers.end() && iequals(trim(eit->second), "100-continue"))
        out.expect_continue = true;

    BodyReader br{fd, avail, wire_in};
    if (out.expect_continue) {
        // tell the client to send the body now
        const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(fd, cont, strlen(cont));
    }

    if (te_chunked) {
        out.chunked_body = true;
        int es = 400;
        if (!decode_chunked(br, out.body, es)) {
            if (br.dead) return false;
            out.error_status = es;
            return true;
        }
        pending = br.buf;
    } else {
        long content_length = 0;
        if (saw_content_length) {
            if (!parse_long(first_content_length, content_length) ||
                content_length < 0)
                return true; // garbage content-length
            if (content_length > MAX_BODY_BYTES) {
                out.error_status = 413;
                return true;
            }
        }
        if (!br.read_bytes((size_t)content_length, out.body)) return false;
        pending = br.buf;
    }
    out.valid = true;
    return true;
}

bool wants_keep_alive(const HttpRequest& req) {
    auto it = req.headers.find("connection");
    std::string c = it == req.headers.end() ? "" : lower(it->second);
    if (req.version == "HTTP/1.1") return c != "close";
    if (req.version == "HTTP/1.0") return c == "keep-alive";
    return false;
}

// send one chunk frame of a chunked response body
static long send_chunk(int fd, const char* p, size_t n) {
    std::string head = to_hex((long)n) + "\r\n";
    long sent = send_all(fd, head.data(), head.size());
    sent += send_all(fd, p, n);
    sent += send_all(fd, "\r\n", 2);
    return sent;
}

long send_response(int fd, HttpResponse& res, bool keep_alive,
                   bool head_only) {
    res.headers["Server"] = "servex";
    res.headers["Connection"] = keep_alive ? "keep-alive" : "close";
    if (keep_alive)
        res.headers["Keep-Alive"] = "timeout=5, max=100";
    long body_len = res.is_file ? res.file_size : (long)res.body.size();
    if (res.chunked) {
        res.headers["Transfer-Encoding"] = "chunked";
        res.headers.erase("Content-Length");
    } else {
        res.headers["Content-Length"] = std::to_string(body_len);
    }
    std::string head = "HTTP/1.1 " + std::to_string(res.status) + " " +
                       res.reason + "\r\n";
    for (auto& kv : res.headers)
        head += kv.first + ": " + kv.second + "\r\n";
    head += "\r\n";
    long sent = send_all(fd, head.data(), head.size());
    if (head_only) return sent;

    if (res.chunked) {
        const size_t step = 8192;
        if (res.is_file) {
            int f = open(res.file_path.c_str(), O_RDONLY);
            if (f >= 0) {
                lseek(f, res.file_offset, SEEK_SET);
                long left = res.file_size;
                char buf[step];
                while (left > 0) {
                    ssize_t r = read(f, buf, left < (long)sizeof(buf)
                                                ? (size_t)left
                                                : sizeof(buf));
                    if (r <= 0) break;
                    sent += send_chunk(fd, buf, r);
                    left -= r;
                }
                close(f);
            }
        } else {
            for (size_t off = 0; off < res.body.size(); off += step) {
                size_t n = res.body.size() - off;
                if (n > step) n = step;
                sent += send_chunk(fd, res.body.data() + off, n);
            }
        }
        sent += send_all(fd, "0\r\n\r\n", 5); // final chunk
        return sent;
    }

    if (res.is_file) {
        int f = open(res.file_path.c_str(), O_RDONLY);
        if (f >= 0) {
            lseek(f, res.file_offset, SEEK_SET);
            long left = res.file_size;
            char buf[8192];
            while (left > 0) {
                ssize_t r = read(f, buf, left < (long)sizeof(buf)
                                             ? (size_t)left
                                             : sizeof(buf));
                if (r <= 0) break;
                sent += send_all(fd, buf, r);
                left -= r;
            }
            close(f);
        }
    } else {
        sent += send_all(fd, res.body.data(), res.body.size());
    }
    return sent;
}
