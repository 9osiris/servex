#pragma once
#include "http.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

struct ServerContext; // defined in server.h

using Handler = std::function<HttpResponse(const HttpRequest&, ServerContext&)>;

// exact path routes plus longest-prefix routes
class Router {
public:
    void add_exact(const std::string& path, Handler h);
    void add_prefix(const std::string& prefix, Handler h);
    Handler find(const std::string& path) const; // empty if no match
    // the route pattern that matched, for stats labels; "" if none
    std::string match_label(const std::string& path) const;
private:
    std::map<std::string, Handler> exact_;
    std::vector<std::pair<std::string, Handler>> prefixes_;
};
