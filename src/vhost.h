#pragma once
#include "config.h"
#include "router.h"
#include <string>
#include <vector>

// one virtual host: its config plus its own route table
struct HostEntry {
    ServerBlock cfg;
    Router router;
};

// build host entries from the parsed config, registering demo routes
// on each one
std::vector<HostEntry> build_hosts(const Config& cfg);
// pick the host for a request: exact name, then wildcard, then default.
// host_header is the raw Host header value (may include a port).
const HostEntry& select_host(const std::vector<HostEntry>& hosts,
                             const std::string& host_header);
// longest-prefix location match inside a server block, nullptr if none
const LocationConfig* find_location(const ServerBlock& server,
                                    const std::string& path);
// "example.com:8080" -> "example.com"
std::string host_without_port(const std::string& host_header);
