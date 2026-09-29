#pragma once
#include "config.h"
#include "http.h"

// middleware chain: rewrites, redirects, basic auth, custom headers.
// runs after location matching, before routing.

// what the chain decided for this request
struct MiddlewareAction {
    bool stop = false;      // send res immediately, skip routing
    bool rewritten = false; // req.path changed, re-resolve the location
    HttpResponse res;
};

// may modify req.path on rewrite. safe to call with loc == nullptr.
MiddlewareAction apply_middleware(const LocationConfig* loc, HttpRequest& req);
