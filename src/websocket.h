#pragma once
#include "http.h"
#include <string>

struct ServerContext;

// validate the handshake and build the 101 response.
// sets res.ws_upgrade on success; error responses otherwise.
HttpResponse ws_handshake(const HttpRequest& req, ServerContext& ctx);

// frame loop: echo text/binary messages, answer pings, handle close.
// pending carries bytes already read past the handshake request.
// returns when the connection closes or times out.
void run_websocket(int fd, ServerContext& ctx, std::string& pending);
