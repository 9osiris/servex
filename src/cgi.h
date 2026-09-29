#pragma once
#include "http.h"
#include <string>

struct ServerContext;
struct LocationConfig;

// run the cgi script backing this request and return its response.
// the script is resolved from docroot + req.path; extra path segments
// become PATH_INFO.
HttpResponse run_cgi(const HttpRequest& req, ServerContext& ctx,
                     const LocationConfig& loc, const std::string& docroot,
                     const std::string& client_ip);
