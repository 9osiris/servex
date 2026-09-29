#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

// live server counters, safe to touch from any worker thread
struct Stats {
    std::atomic<long> requests{0};
    std::atomic<long> bytes_in{0};
    std::atomic<long> bytes_out{0};
    std::atomic<long> active_connections{0};
    std::atomic<long> rejected_429{0};
    std::atomic<long> upstream_errors{0};
    std::atomic<long> cgi_runs{0};
    std::atomic<long> ws_connections{0};
    std::atomic<long> ws_messages{0};
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    // per-route hit counts, guarded by mutex_
    void hit(const std::string& route);
    long route_count(const std::string& route) const;
    long uptime_seconds() const;
    // full admin json, includes the legacy server/uptime_seconds/requests keys
    std::string to_json(long workers = 0, long queue_depth = 0) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, long> route_hits_;
};
