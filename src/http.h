#pragma once
#include <map>
#include <string>

// parsed http/1.x request
struct HttpRequest {
    std::string method;
    std::string target;   // raw request target
    std::string path;     // url-decoded, query stripped
    std::string query;
    std::string version;  // e.g. "HTTP/1.1"
    std::map<std::string, std::string> headers; // names lowercased
    std::string body;
    bool valid = false;
    int error_status = 400; // status to send when !valid
    bool chunked_body = false;   // body arrived with transfer-encoding: chunked
    bool expect_continue = false; // client sent expect: 100-continue
};

// response ready to send
struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
    bool is_file = false;
    std::string file_path;
    long file_size = 0;
    long file_offset = 0; // first byte served from file_path
    bool chunked = false; // stream the body with chunked transfer encoding
    bool ws_upgrade = false; // 101: hand the socket to the websocket loop
};

// false only when the connection died (eof or socket error).
// pending carries over bytes from a previous pipelined request.
// wire_in counts raw bytes read from the socket, for stats.
bool read_request(int fd, std::string& pending, HttpRequest& out,
                  long* wire_in = nullptr);
// returns bytes written. head_only sends headers but no body (for HEAD).
long send_response(int fd, HttpResponse& res, bool keep_alive,
                   bool head_only = false);
HttpResponse make_error(int status, const std::string& message);
HttpResponse make_options_response(const HttpRequest& req);
std::string reason_phrase(int status);
std::string url_decode(const std::string& s);
bool wants_keep_alive(const HttpRequest& req);
// true when the request used transfer-encoding: chunked
bool uses_chunked(const HttpRequest& req);
