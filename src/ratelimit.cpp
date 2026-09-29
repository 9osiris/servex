// token bucket rate limiter
#include "ratelimit.h"
#include <chrono>

static long long now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

bool RateLimiter::allow(const std::string& key, double rate, long burst,
                        long& retry_after) {
    if (rate <= 0 || burst <= 0) return true;
    long long now = now_ns();
    std::lock_guard<std::mutex> lock(mutex_);
    if (++ops_ % 1024 == 0) sweep();
    Bucket& b = buckets_[key];
    if (!b.initialized) {
        b.tokens = (double)burst;
        b.last_ns = now;
        b.initialized = true;
    } else {
        double elapsed = (double)(now - b.last_ns) / 1e9;
        b.tokens += elapsed * rate;
        if (b.tokens > (double)burst) b.tokens = (double)burst;
        b.last_ns = now;
    }
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    double wait_s = (1.0 - b.tokens) / rate;
    retry_after = (long)(wait_s + 0.999); // round up
    if (retry_after < 1) retry_after = 1;
    return false;
}

void RateLimiter::sweep() {
    long long now = now_ns();
    const long long idle_ns = 10LL * 60 * 1000000000LL; // 10 minutes
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        if (now - it->second.last_ns > idle_ns)
            it = buckets_.erase(it);
        else
            ++it;
    }
}
