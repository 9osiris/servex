#pragma once
#include "http.h"
#include <string>

// serve a request path from the document root, honoring conditional
// headers, byte ranges, and HEAD
HttpResponse serve_file_request(const std::string& docroot,
                                const HttpRequest& req, bool dir_listing);
std::string mime_type(const std::string& filename);
