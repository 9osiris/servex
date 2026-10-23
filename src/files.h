#pragma once
#include "gzip.h"
#include "http.h"
#include <string>

// serve a request path from the document root, honoring conditional
// headers, byte ranges, HEAD, and optional gzip content encoding
HttpResponse serve_file_request(const std::string& docroot,
                                const HttpRequest& req, bool dir_listing,
                                const GzipOpts& gz);
std::string mime_type(const std::string& filename);









