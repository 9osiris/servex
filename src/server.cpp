// tcp accept loop, connections dispatched to a worker thread pool
#include "server.h"
#include "cgi.h"
#include "demo.h"
#include "files.h"
#include "http.h"
#include "log.h"
#include "middleware.h"
#include "proxy.h"
#include "websocket.h"
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const int KEEP_ALIVE_TIMEOUT = 5; // seconds of idle before we close
static const int KEEP_ALIVE_MAX = 100;   // requests per connection

// true when the socket has data within timeout_sec
static bool wait_readable(int fd, int timeout_sec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    return select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
}

Server::Server(const Config& cfg) : cfg_(cfg), pool_(cfg.workers) {
    ctx_.port = cfg.port;
    ctx_.log_path = cfg.log_path;
    ctx_.log_max_bytes = cfg.log_max_bytes;
    ctx_.log_archives = cfg.log_archives;
    ctx_.pool = &pool_;
}

bool Server::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("socket");
        return false;
    }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ctx_.port);
    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return false;
    }
    if (listen(listen_fd_, 128) < 0) {
        perror("listen");
        return false;
    }
    return true;
}

void Server::request_stop() {
    ctx_.stopping = true;
}

// one full connection: parse requests until close, idle timeout, or cap
static void handle_connection(int fd, ServerContext* ctx,
                              std::string client_ip) {
    ctx->stats.active_connections++;
    bool alive = true;
    int count = 0;
    std::string pending; // leftover bytes from pipelined requests
    long wire_in = 0;    // raw bytes read from this socket
    while (alive && count < KEEP_ALIVE_MAX && !ctx->stopping) {
        if (pending.empty() && !wait_readable(fd, KEEP_ALIVE_TIMEOUT)) break;
        long before = wire_in;
        HttpRequest req;
        if (!read_request(fd, pending, req, &wire_in)) break; // went away
        ctx->stats.bytes_in += wire_in - before;
        ctx->stats.requests++;
        count++;
        HttpResponse res;
        bool head_only = false;
        std::string route_label = "invalid";
        std::string log_path = ctx->log_path;
        if (!req.valid) {
            res = make_error(req.error_status, "the request could not be parsed");
            alive = false;
        } else if (req.method == "OPTIONS") {
            alive = wants_keep_alive(req);
            res = make_options_response(req);
            route_label = "OPTIONS";
        } else {
            alive = wants_keep_alive(req);
            head_only = (req.method == "HEAD");
            std::string host_hdr;
            auto hit = req.headers.find("host");
            if (hit != req.headers.end()) host_hdr = hit->second;
            const HostEntry& host = select_host(ctx->hosts, host_hdr);
            if (!host.cfg.log_path.empty()) log_path = host.cfg.log_path;
            // rate limits: server-wide first, then the location's own
            long retry_after = 0;
            bool limited = false;
            if (host.cfg.rate > 0 && host.cfg.burst > 0 &&
                !ctx->limiter.allow(client_ip + "|server", host.cfg.rate,
                                    host.cfg.burst, retry_after))
                limited = true;
            const LocationConfig* pre_loc =
                limited ? nullptr : find_location(host.cfg, req.path);
            if (!limited && pre_loc && pre_loc->rate > 0 &&
                pre_loc->burst > 0 &&
                !ctx->limiter.allow(client_ip + "|" + pre_loc->match,
                                    pre_loc->rate, pre_loc->burst,
                                    retry_after))
                limited = true;
            if (limited) {
                ctx->stats.rejected_429++;
                route_label = "ratelimited";
                res = make_error(429, "rate limit exceeded, slow down");
                res.headers["Retry-After"] = std::to_string(retry_after);
            } else {
                // middleware chain: rewrites re-resolve the location
                const LocationConfig* loc = nullptr;
                bool mw_handled = false;
                for (int pass = 0; pass < 4; pass++) {
                    loc = find_location(host.cfg, req.path);
                    MiddlewareAction act = apply_middleware(loc, req);
                    if (act.stop) {
                        res = act.res;
                        mw_handled = true;
                        break;
                    }
                    if (!act.rewritten) break;
                }
                if (mw_handled) {
                    route_label = loc ? "location:" + loc->match : "middleware";
                } else if (loc && !loc->proxy_pass.empty()) {
                    route_label = "proxy:" + loc->match;
                    res = proxy_request(req, *ctx, *loc, client_ip);
                } else if (loc && loc->cgi) {
                    route_label = "cgi:" + loc->match;
                    res = run_cgi(req, *ctx, *loc, host.cfg.docroot,
                                  client_ip);
                } else if (loc && loc->websocket) {
                    route_label = "websocket:" + loc->match;
                    res = ws_handshake(req, *ctx);
                } else {
                    Handler h = host.router.find(req.path);
                    if (h) {
                        route_label = req.path;
                        res = h(req, *ctx);
                    } else {
                        route_label = "static";
                        res = serve_file_request(host.cfg.docroot, req,
                                                 host.cfg.dir_listing);
                    }
                    if (loc)
                        for (auto& ah : loc->add_headers)
                            res.headers[ah.first] = ah.second;
                }
            }
        }
        ctx->stats.hit(route_label);
        if (res.ws_upgrade) {
            // hijack the socket: send the 101, then run the frame loop
            long bytes = send_response(fd, res, false, false);
            ctx->stats.bytes_out += bytes;
            log_access(*ctx, log_path, client_ip, req, res.status, bytes);
            run_websocket(fd, *ctx, pending);
            break;
        }
        bool ka = alive && count < KEEP_ALIVE_MAX;
        long bytes = send_response(fd, res, ka, head_only);
        ctx->stats.bytes_out += bytes;
        log_access(*ctx, log_path, client_ip, req, res.status, bytes);
    }
    close(fd);
    ctx->stats.active_connections--;
}

void Server::run() {
    ctx_.start = std::chrono::steady_clock::now();
    ctx_.stats.start = ctx_.start;
    ctx_.hosts = build_hosts(cfg_);
    pool_.start();
    std::cout << "servex listening on port " << ctx_.port << " with "
              << pool_.worker_count() << " workers" << std::endl;
    while (!ctx_.stopping) {
        // poll the listen socket so request_stop() wakes us promptly
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listen_fd_, &set);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000;
        if (select(listen_fd_ + 1, &set, nullptr, nullptr, &tv) <= 0)
            continue;
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = accept(listen_fd_, (struct sockaddr*)&cli, &len);
        if (fd < 0) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        std::string client_ip = ip;
        if (!pool_.submit(
                [fd, client_ip, ctx = &ctx_]() { handle_connection(fd, ctx, client_ip); })) {
            close(fd); // shutting down, refuse the connection
        }
    }
    pool_.stop();
    close(listen_fd_);
    std::cout << "servex stopped" << std::endl;
}
