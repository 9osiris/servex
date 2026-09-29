// demo route handlers
#include "demo.h"
#include "server.h"
#include <chrono>

static HttpResponse hello_handler(const HttpRequest&, ServerContext&) {
    HttpResponse r;
    r.headers["Content-Type"] = "text/plain";
    r.body = "Hello from servex\n";
    return r;
}

static HttpResponse echo_handler(const HttpRequest& req, ServerContext&) {
    HttpResponse r;
    r.headers["Content-Type"] = "text/plain";
    std::string out = "method: " + req.method + "\n";
    out += "path: " + req.path + "\n";
    out += "query: " + req.query + "\n";
    out += "headers:\n";
    for (auto& kv : req.headers)
        out += "  " + kv.first + ": " + kv.second + "\n";
    out += "body:\n" + req.body + "\n";
    r.body = out;
    return r;
}

static HttpResponse status_handler(const HttpRequest&, ServerContext& ctx) {
    HttpResponse r;
    r.headers["Content-Type"] = "application/json";
    long workers = ctx.pool ? ctx.pool->worker_count() : 0;
    long depth = ctx.pool ? ctx.pool->queue_depth() : 0;
    r.body = ctx.stats.to_json(workers, depth) + "\n";
    return r;
}

void register_demo_routes(Router& r) {
    r.add_exact("/hello", hello_handler);
    r.add_exact("/echo", echo_handler);
    r.add_exact("/status", status_handler);
}
