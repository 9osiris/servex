// virtual host selection and per-host route tables
#include "vhost.h"
#include "demo.h"
#include "util.h"

std::string host_without_port(const std::string& host_header) {
    std::string h = lower(trim(host_header));
    size_t c = h.rfind(':');
    // strip a port, but leave ipv6 literals alone
    if (c != std::string::npos && h.find(':') == c) h = h.substr(0, c);
    if (!h.empty() && h.front() == '[' && h.back() == ']')
        h = h.substr(1, h.size() - 2);
    return h;
}

std::vector<HostEntry> build_hosts(const Config& cfg) {
    std::vector<HostEntry> hosts;
    for (auto& s : cfg.servers) {
        HostEntry e;
        e.cfg = s;
        register_demo_routes(e.router);
        hosts.push_back(std::move(e));
    }
    return hosts;
}

// does a server name pattern match this host?
static bool name_matches(const std::string& pattern, const std::string& host) {
    std::string p = lower(pattern);
    if (p == host) return true;
    if (starts_with(p, "*.")) {
        std::string suffix = p.substr(1); // ".example.com"
        if (ends_with(host, suffix) && host.size() > suffix.size())
            return true;
    }
    return false;
}

const HostEntry& select_host(const std::vector<HostEntry>& hosts,
                             const std::string& host_header) {
    static HostEntry empty; // never returned when hosts is non-empty
    if (hosts.empty()) return empty;
    std::string host = host_without_port(host_header);
    if (!host.empty()) {
        // exact names first
        for (auto& h : hosts)
            for (auto& n : h.cfg.hosts)
                if (lower(n) == host && n.find('*') == std::string::npos)
                    return h;
        // then wildcards
        for (auto& h : hosts)
            for (auto& n : h.cfg.hosts)
                if (name_matches(n, host)) return h;
    }
    return hosts[0]; // default server
}

const LocationConfig* find_location(const ServerBlock& server,
                                    const std::string& path) {
    const LocationConfig* best = nullptr;
    size_t best_len = 0;
    for (auto& l : server.locations) {
        if (l.match.empty()) continue;
        if (path.compare(0, l.match.size(), l.match) == 0 &&
            l.match.size() > best_len) {
            best = &l;
            best_len = l.match.size();
        }
    }
    return best;
}
