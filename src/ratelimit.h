#pragma once
#include <map>
#include <mutex>
#include <string>

// token bucket rate limiter, keyed per client (and per location).
// each key gets its own bucket: burst tokens max, refilled at rate/sec.
class RateLimiter {
public:
    // true when the request may proceed. on false, retry_after holds
    // the seconds until the next token is expected.
    bool allow(const std::string& key, double rate, long burst,
               long& retry_after);

private:
    struct Bucket {
        double tokens = 0;
        long long last_ns = 0;
        bool initialized = false;
    };
    void sweep(); // drop buckets idle for a while, called with mutex held

    std::mutex mutex_;
    std::map<std::string, Bucket> buckets_;
    long ops_ = 0;
};
