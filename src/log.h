#pragma once
#include <string>

struct HttpRequest;
struct ServerContext;

// apache combined-ish access log line
void log_access(ServerContext& ctx, const std::string& log_path,
                const std::string& client_ip, const HttpRequest& req,
                int status, long bytes_sent);
