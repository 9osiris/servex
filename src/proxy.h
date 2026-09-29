#pragma once
#include "http.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct ServerContext;
struct LocationConfig;

// pooled keep-alive tcp connections to upstream servers,
// keyed by "host:port"
class UpstreamPool {
public:
    UpstreamPool(int max_per_upstream = 8, int idle_seconds = 60);
    // connected fd, or -1 on failure. pending_out carries bytes the
    // previous user left unread on a reused connection.
    int acquire(const std::string& host, int port, int timeout_sec,
                std::string& pending_out, bool& from_pool);
    // hand the fd back; closed instead when unhealthy, stale, or full
    void release(const std::string& host, int port, int fd,
                 const std::string& pending, bool healthy);

private:
    struct Entry {
        int fd;
        long long last_used_ns;
        std::string pending;
    };
    std::mutex mutex_;
    std::map<std::string, std::vector<Entry>> pool_;
    int max_per_;
    long long idle_ns_;
};

// forward one request to the location's proxy_pass upstream and
// return its response
HttpResponse proxy_request(const HttpRequest& req, ServerContext& ctx,
                           const LocationConfig& loc,
                           const std::string& client_ip);
