// rfc 6455 websocket: handshake, frame echo, ping/pong, close
#include "websocket.h"
#include "crypto.h"
#include "server.h"
#include "util.h"
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static const std::string WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const size_t WS_MAX_MESSAGE = 1024 * 1024; // 1mb
static const int WS_IDLE_TIMEOUT = 60;

// opcodes
static const int OP_CONT = 0x0;
static const int OP_TEXT = 0x1;
static const int OP_BINARY = 0x2;
static const int OP_CLOSE = 0x8;
static const int OP_PING = 0x9;
static const int OP_PONG = 0xa;

static bool has_token(const HttpRequest& req, const std::string& name,
                      const std::string& token) {
    auto it = req.headers.find(name);
    if (it == req.headers.end()) return false;
    for (auto& part : split(it->second, ',')) {
        if (lower(trim(part)) == token) return true;
    }
    return false;
}

HttpResponse ws_handshake(const HttpRequest& req, ServerContext& ctx) {
    auto fail = [](int code, const std::string& msg) {
        return make_error(code, msg);
    };
    if (req.method != "GET") return fail(405, "websocket needs a get request");
    auto up = req.headers.find("upgrade");
    if (up == req.headers.end() || lower(trim(up->second)) != "websocket")
        return fail(426, "upgrade to websocket required");
    if (!has_token(req, "connection", "upgrade"))
        return fail(400, "connection header must include upgrade");
    auto key = req.headers.find("sec-websocket-key");
    if (key == req.headers.end() || key->second.empty())
        return fail(400, "missing sec-websocket-key");
    auto ver = req.headers.find("sec-websocket-version");
    if (ver == req.headers.end() || trim(ver->second) != "13")
        return fail(400, "unsupported websocket version");

    HttpResponse res;
    res.status = 101;
    res.reason = "Switching Protocols";
    res.headers["Upgrade"] = "websocket";
    res.headers["Connection"] = "Upgrade";
    res.headers["Sec-WebSocket-Accept"] =
        base64_encode(sha1_raw(trim(key->second) + WS_GUID));
    res.ws_upgrade = true;
    ctx.stats.ws_connections++;
    return res;
}

// frame read/write helpers

static bool wait_readable(int fd, int timeout_sec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    return select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
}

// pull exactly n bytes from pending/the socket, false on timeout/eof
static bool read_n(int fd, std::string& pending, size_t n, std::string& out,
                   long& wire_in) {
    out.clear();
    out.reserve(n);
    while (out.size() < n) {
        if (pending.empty()) {
            if (!wait_readable(fd, WS_IDLE_TIMEOUT)) return false;
            char tmp[8192];
            ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;
            wire_in += r;
            pending.append(tmp, r);
        }
        size_t take = pending.size();
        if (take > n - out.size()) take = n - out.size();
        out.append(pending, 0, take);
        pending.erase(0, take);
    }
    return true;
}

static bool send_all(int fd, const char* p, size_t n, long& wire_out) {
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        wire_out += r;
        p += r;
        n -= r;
    }
    return true;
}

// one unmasked server frame
static bool send_frame(int fd, int opcode, const std::string& payload,
                       long& wire_out) {
    std::string out;
    out += (char)(0x80 | opcode); // fin set
    size_t n = payload.size();
    if (n < 126) {
        out += (char)n;
    } else if (n < 65536) {
        out += (char)126;
        out += (char)((n >> 8) & 0xff);
        out += (char)(n & 0xff);
    } else {
        out += (char)127;
        for (int i = 7; i >= 0; i--) out += (char)((n >> (8 * i)) & 0xff);
    }
    out += payload;
    return send_all(fd, out.data(), out.size(), wire_out);
}

static bool send_close(int fd, int code, long& wire_out) {
    std::string payload;
    payload += (char)((code >> 8) & 0xff);
    payload += (char)(code & 0xff);
    return send_frame(fd, OP_CLOSE, payload, wire_out);
}

struct WsFrame {
    bool fin;
    int opcode;
    bool masked;
    std::string payload;
};

// read one client frame; ok=false means the connection is dead
static bool read_frame(int fd, std::string& pending, WsFrame& f, bool& ok,
                       long& wire_in) {
    ok = true;
    std::string hdr;
    if (!read_n(fd, pending, 2, hdr, wire_in)) {
        ok = false;
        return false;
    }
    unsigned char b0 = hdr[0], b1 = hdr[1];
    f.fin = (b0 & 0x80) != 0;
    f.opcode = b0 & 0x0f;
    f.masked = (b1 & 0x80) != 0;
    bool masked = f.masked;
    uint64_t len = b1 & 0x7f;
    if (len == 126) {
        std::string ext;
        if (!read_n(fd, pending, 2, ext, wire_in)) {
            ok = false;
            return false;
        }
        len = ((uint64_t)(unsigned char)ext[0] << 8) |
              (unsigned char)ext[1];
    } else if (len == 127) {
        std::string ext;
        if (!read_n(fd, pending, 8, ext, wire_in)) {
            ok = false;
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | (unsigned char)ext[i];
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        std::string m;
        if (!read_n(fd, pending, 4, m, wire_in)) {
            ok = false;
            return false;
        }
        memcpy(mask, m.data(), 4);
    }
    if (!masked) return true; // unmasked: caller fails the connection
    if (len > WS_MAX_MESSAGE) {
        ok = false;
        return false;
    }
    if (!read_n(fd, pending, (size_t)len, f.payload, wire_in)) {
        ok = false;
        return false;
    }
    for (size_t i = 0; i < f.payload.size(); i++)
        f.payload[i] ^= mask[i % 4];
    return true;
}

void run_websocket(int fd, ServerContext& ctx, std::string& pending) {
    long wire_in = 0, wire_out = 0;
    std::string message; // reassembled fragmented message
    int message_op = -1;
    bool open = true;
    while (open && !ctx.stopping) {
        WsFrame f;
        bool ok = true;
        if (!read_frame(fd, pending, f, ok, wire_in)) break;
        if (!f.masked) { // rfc 6455: clients must mask every frame
            send_close(fd, 1002, wire_out);
            break;
        }
        if (f.opcode >= OP_CLOSE) {
            // control frames must be final and short
            if (!f.fin || f.payload.size() > 125) break;
            if (f.opcode == OP_CLOSE) {
                int code = 1000;
                if (f.payload.size() >= 2)
                    code = ((unsigned char)f.payload[0] << 8) |
                           (unsigned char)f.payload[1];
                send_close(fd, code, wire_out);
                break;
            }
            if (f.opcode == OP_PING) {
                send_frame(fd, OP_PONG, f.payload, wire_out);
                continue;
            }
            continue; // pong or unknown: ignore
        }
        // data frames
        if (f.opcode == OP_TEXT || f.opcode == OP_BINARY) {
            if (message_op != -1) break; // new message before the last ended
            message_op = f.opcode;
            message.clear();
        } else if (f.opcode == OP_CONT) {
            if (message_op == -1) break; // continuation with no message
        } else {
            break; // reserved opcode
        }
        if (message.size() + f.payload.size() > WS_MAX_MESSAGE) {
            send_close(fd, 1009, wire_out);
            break;
        }
        message += f.payload;
        if (f.fin) {
            send_frame(fd, message_op, message, wire_out);
            ctx.stats.ws_messages++;
            message_op = -1;
            message.clear();
        }
    }
    ctx.stats.bytes_in += wire_in;
    ctx.stats.bytes_out += wire_out;
}
