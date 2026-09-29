#pragma once
#include "cgi.h"
#include "config.h"
#include "proxy.h"
#include "ratelimit.h"
#include "router.h"
#include "stats.h"
#include "threadpool.h"
#include "vhost.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

// shared state across connection workers
struct ServerContext {
    int port = 8080;
    std::string log_path; // global default, empty means stdout
    long log_max_bytes = 0; // rotate access logs past this size, 0 = off
    int log_archives = 3;   // archived copies kept per log file
    std::chrono::steady_clock::time_point start;
    std::mutex log_mutex;
    std::vector<HostEntry> hosts;
    Stats stats;
    RateLimiter limiter;
    UpstreamPool upstreams;
    ThreadPool* pool = nullptr; // set by Server::run
    std::atomic<bool> stopping{false};
};

// tcp server: bind, listen, accept loop, dispatch to the thread pool
class Server {
public:
    explicit Server(const Config& cfg);
    bool start(); // bind + listen, false on error
    void run();   // accept loop, returns on request_stop
    void request_stop();
private:
    ServerContext ctx_;
    Config cfg_;
    ThreadPool pool_;
    int listen_fd_ = -1;
};
