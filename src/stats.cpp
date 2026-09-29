// live server counters
#include "stats.h"
#include <chrono>
#include <cstdio>
#include <sstream>

void Stats::hit(const std::string& route) {
    std::lock_guard<std::mutex> lock(mutex_);
    route_hits_[route]++;
}

long Stats::route_count(const std::string& route) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = route_hits_.find(route);
    return it == route_hits_.end() ? 0 : it->second;
}

long Stats::uptime_seconds() const {
    using namespace std::chrono;
    return duration_cast<seconds>(steady_clock::now() - start).count();
}

// minimal json string escaper for route labels
static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if ((unsigned char)c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

std::string Stats::to_json(long workers, long queue_depth) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream o;
    o << "{\"server\":\"servex\"";
    o << ",\"uptime_seconds\":" << uptime_seconds();
    long n = requests.load();
    o << ",\"requests\":" << n; // legacy key, kept for old clients
    o << ",\"requests_total\":" << n;
    o << ",\"bytes_in\":" << bytes_in.load();
    o << ",\"bytes_out\":" << bytes_out.load();
    o << ",\"active_connections\":" << active_connections.load();
    o << ",\"workers\":" << workers;
    o << ",\"queue_depth\":" << queue_depth;
    o << ",\"rejected_429\":" << rejected_429.load();
    o << ",\"upstream_errors\":" << upstream_errors.load();
    o << ",\"cgi_runs\":" << cgi_runs.load();
    o << ",\"ws_connections\":" << ws_connections.load();
    o << ",\"ws_messages\":" << ws_messages.load();
    o << ",\"routes\":{";
    bool first = true;
    for (auto& kv : route_hits_) {
        if (!first) o << ",";
        first = false;
        o << "\"" << json_escape(kv.first) << "\":" << kv.second;
    }
    o << "}}";
    return o.str();
}
