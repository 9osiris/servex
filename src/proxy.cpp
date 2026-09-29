// reverse proxy with pooled keep-alive upstream connections
#include "proxy.h"
#include "config.h"
#include "server.h"
#include "util.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const size_t MAX_UPSTREAM_HEADER = 65536;
static const long MAX_UPSTREAM_BODY = 8 * 1024 * 1024;
static const int CONNECT_TIMEOUT = 10;
static const int READ_TIMEOUT = 30;

static long long now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

UpstreamPool::UpstreamPool(int max_per_upstream, int idle_seconds)
    : max_per_(max_per_upstream),
      idle_ns_((long long)idle_seconds * 1000000000LL) {}

int UpstreamPool::acquire(const std::string& host, int port, int timeout_sec,
                          std::string& pending_out, bool& from_pool) {
    std::string k = host + ":" + std::to_string(port);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pool_.find(k);
        if (it != pool_.end()) {
            long long now = now_ns();
            while (!it->second.empty()) {
                Entry e = it->second.back();
                it->second.pop_back();
                if (now - e.last_used_ns > idle_ns_) {
                    close(e.fd); // idle too long, drop it
                    continue;
                }
                pending_out = e.pending;
                from_pool = true;
                return e.fd;
            }
        }
    }
    from_pool = false;
    pending_out.clear();
    return -1; // no pooled connection; caller dials a fresh one
}

void UpstreamPool::release(const std::string& host, int port, int fd,
                           const std::string& pending, bool healthy) {
    if (!healthy) {
        close(fd);
        return;
    }
    std::string k = host + ":" + std::to_string(port);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& vec = pool_[k];
    if ((int)vec.size() >= max_per_) {
        close(fd);
        return;
    }
    vec.push_back({fd, now_ns(), pending});
}

// dial host:port with a connect timeout, -1 on failure
static int dial(const std::string& host, int port, int timeout_sec) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                    &res) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        if (select(fd + 1, nullptr, &set, nullptr, &tv) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags); // back to blocking
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static bool parse_upstream(const std::string& s, std::string& host,
                           int& port) {
    size_t c = s.rfind(':');
    if (c == std::string::npos || c + 1 >= s.size()) return false;
    host = trim(s.substr(0, c));
    long p;
    if (!parse_long(trim(s.substr(c + 1)), p) || p <= 0 || p > 65535)
        return false;
    port = (int)p;
    return !host.empty();
}

static bool send_all_up(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

// recv with an idle timeout, false on timeout/error/eof
static bool recv_up(int fd, std::string& buf, int timeout_sec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) return false;
    char tmp[8192];
    ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
    if (r <= 0) return false;
    buf.append(tmp, r);
    return true;
}

struct UpstreamResponse {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> headers;
    std::string body;
    bool chunked = false;
    bool keep_alive = false;
};

// headers we never forward upstream
static bool is_hop_header(const std::string& name) {
    return name == "connection" || name == "keep-alive" ||
           name == "proxy-authenticate" || name == "proxy-authorization" ||
           name == "te" || name == "trailer" || name == "transfer-encoding" ||
           name == "upgrade";
}

static bool send_upstream_request(int fd, const HttpRequest& req,
                                  const std::string& up_host, int up_port,
                                  const std::string& client_ip) {
    std::string out = req.method + " " + req.target + " HTTP/1.1\r\n";
    bool has_host = false;
    for (auto& kv : req.headers) {
        if (is_hop_header(kv.first)) continue;
        if (kv.first == "host") has_host = true;
        out += kv.first + ": " + kv.second + "\r\n";
    }
    if (!has_host) out += "host: " + up_host + ":" + std::to_string(up_port) + "\r\n";
    out += "connection: keep-alive\r\n";
    auto fwd = req.headers.find("x-forwarded-for");
    out += "x-forwarded-for: ";
    if (fwd != req.headers.end()) out += fwd->second + ", ";
    out += client_ip + "\r\n";
    out += "x-real-ip: " + client_ip + "\r\n";
    out += "content-length: " + std::to_string(req.body.size()) + "\r\n";
    out += "\r\n";
    if (!send_all_up(fd, out.data(), out.size())) return false;
    if (!req.body.empty() &&
        !send_all_up(fd, req.body.data(), req.body.size()))
        return false;
    return true;
}

// read exactly n bytes (pending first), false on timeout/eof
static bool read_n_up(int fd, std::string& pending, size_t n,
                      std::string& out) {
    out.clear();
    out.reserve(n);
    while (out.size() < n) {
        if (pending.empty() && !recv_up(fd, pending, READ_TIMEOUT))
            return false;
        size_t take = pending.size();
        if (take > n - out.size()) take = n - out.size();
        out.append(pending, 0, take);
        pending.erase(0, take);
    }
    return true;
}

// read one crlf line (pending first), false on timeout/eof/oversize
static bool read_line_up(int fd, std::string& pending, std::string& line) {
    while (true) {
        size_t p = pending.find("\r\n");
        if (p != std::string::npos) {
            line = pending.substr(0, p);
            pending.erase(0, p + 2);
            return true;
        }
        if (pending.size() > MAX_UPSTREAM_HEADER) return false;
        if (!recv_up(fd, pending, READ_TIMEOUT)) return false;
    }
}

static bool read_upstream_response(int fd, std::string& pending,
                                   UpstreamResponse& res) {
    // status line + headers
    while (pending.find("\r\n\r\n") == std::string::npos) {
        if (pending.size() > MAX_UPSTREAM_HEADER) return false;
        if (!recv_up(fd, pending, READ_TIMEOUT)) return false;
    }
    size_t hlen = pending.find("\r\n\r\n") + 4;
    std::string head = pending.substr(0, hlen);
    pending.erase(0, hlen);
    size_t eol = head.find("\r\n");
    std::string status_line = head.substr(0, eol);
    std::vector<std::string> parts = split(status_line, ' ');
    if (parts.size() < 2 || parts[0].compare(0, 5, "HTTP/") != 0)
        return false;
    long code;
    if (!parse_long(parts[1], code)) return false;
    res.status = (int)code;
    res.reason = parts.size() > 2 ? status_line.substr(parts[0].size() + 1 +
                                                       parts[1].size() + 1)
                                  : reason_phrase(res.status);
    size_t pos = eol + 2;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos || nl == pos) break;
        std::string line = head.substr(pos, nl - pos);
        pos = nl + 2;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        res.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    }

    bool is_11 = parts[0] == "HTTP/1.1";
    auto conn = res.headers.find("connection");
    std::string conn_v = conn == res.headers.end() ? "" : lower(conn->second);
    res.keep_alive = is_11 ? (conn_v != "close") : (conn_v == "keep-alive");

    bool no_body = (res.status == 204 || res.status == 304 ||
                    (res.status >= 100 && res.status < 200));
    auto te = res.headers.find("transfer-encoding");
    bool chunked =
        te != res.headers.end() && lower(te->second).find("chunked") !=
                                      std::string::npos;
    if (no_body) return true;
    if (chunked) {
        res.chunked = true;
        std::string body;
        while (true) {
            std::string line;
            if (!read_line_up(fd, pending, line)) return false;
            size_t semi = line.find(';');
            long n;
            if (!parse_hex(semi == std::string::npos ? line
                                                     : line.substr(0, semi),
                           n) ||
                n < 0)
                return false;
            if ((long)body.size() + n > MAX_UPSTREAM_BODY) return false;
            if (n == 0) {
                while (read_line_up(fd, pending, line) && !line.empty()) {
                }
                break;
            }
            std::string chunk;
            if (!read_n_up(fd, pending, (size_t)n, chunk)) return false;
            body += chunk;
            if (!read_line_up(fd, pending, line) || !line.empty())
                return false;
        }
        res.body = body;
        return true;
    }
    auto cl = res.headers.find("content-length");
    if (cl != res.headers.end()) {
        long n;
        if (!parse_long(cl->second, n) || n < 0 || n > MAX_UPSTREAM_BODY)
            return false;
        return read_n_up(fd, pending, (size_t)n, res.body);
    }
    // no framing: read until close, and do not pool this connection
    res.keep_alive = false;
    while (recv_up(fd, pending, READ_TIMEOUT)) {
        if ((long)res.body.size() + (long)pending.size() > MAX_UPSTREAM_BODY)
            return false;
        res.body += pending;
        pending.clear();
    }
    return true;
}

HttpResponse proxy_request(const HttpRequest& req, ServerContext& ctx,
                           const LocationConfig& loc,
                           const std::string& client_ip) {
    std::string up_host;
    int up_port;
    if (!parse_upstream(loc.proxy_pass, up_host, up_port)) {
        ctx.stats.upstream_errors++;
        return make_error(500, "bad proxy_pass target");
    }
    int fd = -1;
    std::string pending;
    bool from_pool = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        fd = ctx.upstreams.acquire(up_host, up_port, CONNECT_TIMEOUT, pending,
                                   from_pool);
        if (fd < 0) fd = dial(up_host, up_port, CONNECT_TIMEOUT);
        if (fd < 0) break;
        if (send_upstream_request(fd, req, up_host, up_port, client_ip)) {
            UpstreamResponse ures;
            if (read_upstream_response(fd, pending, ures) && ures.status > 0) {
                HttpResponse res;
                res.status = ures.status;
                res.reason = ures.reason.empty()
                                 ? reason_phrase(ures.status)
                                 : ures.reason;
                for (auto& kv : ures.headers) {
                    if (is_hop_header(kv.first)) continue;
                    res.headers[kv.first] = kv.second;
                }
                res.body = ures.body;
                if (ures.chunked) {
                    // re-stream as chunked to the client
                    res.headers.erase("content-length");
                    res.chunked = true;
                }
                ctx.upstreams.release(up_host, up_port, fd, pending,
                                      ures.keep_alive);
                return res;
            }
        }
        // send or read failed: drop the connection
        ctx.upstreams.release(up_host, up_port, fd, "", false);
        fd = -1;
        if (attempt == 0 && from_pool) continue; // retry once on a fresh dial
        break;
    }
    ctx.stats.upstream_errors++;
    return make_error(502, "the upstream server did not respond");
}
