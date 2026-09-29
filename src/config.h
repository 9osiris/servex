#pragma once
#include <map>
#include <string>
#include <vector>

// per-location settings inside a server block
struct LocationConfig {
    std::string match; // prefix this location applies to, e.g. "/api/"
    std::string proxy_pass; // "host:port" to reverse proxy to, "" = off
    bool cgi = false;
    bool websocket = false; // allow websocket upgrades under this prefix
    std::string auth_realm; // "" = no auth required
    std::string auth_file;  // htpasswd-style credentials file
    std::vector<std::pair<std::string, std::string>> add_headers;
    std::string rewrite_pattern; // regex matched against the path
    std::string rewrite_replacement; // may use $1..$9
    std::string redirect_to; // "" = no redirect
    int redirect_code = 301;
    double rate = 0; // requests per second, 0 = unlimited
    long burst = 0;
};

// one virtual host
struct ServerBlock {
    std::vector<std::string> hosts; // names, "*.example.com" wildcards
    std::string docroot = "./www";
    bool dir_listing = true;
    std::string log_path; // "" falls back to the global log
    double rate = 0;
    long burst = 0;
    std::vector<LocationConfig> locations;
};

// whole server configuration
struct Config {
    int port = 8080;
    int workers = 32;
    std::string log_path; // global access log, "" = stdout
    long log_max_bytes = 0; // 0 = no rotation
    int log_archives = 3;
    std::map<std::string, std::string> vars;
    std::vector<ServerBlock> servers;
    std::vector<std::string> errors; // non-fatal parse problems
};

Config load_config(const std::string& path);
