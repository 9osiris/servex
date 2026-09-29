// servex entry point
#include "config.h"
#include "server.h"
#include <csignal>
#include <iostream>

static Server* g_server = nullptr;

// sigint/sigterm ask the accept loop to drain and stop cleanly
static void on_shutdown_signal(int) {
    if (g_server) g_server->request_stop();
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN); // never die on a closed socket
    signal(SIGINT, on_shutdown_signal);
    signal(SIGTERM, on_shutdown_signal);
    std::string conf_path = "servex.conf";
    int port_override = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port_override = std::stoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: servex [config_file] [--port N]\n";
            return 0;
        } else {
            conf_path = a;
        }
    }
    Config cfg = load_config(conf_path);
    if (port_override > 0) cfg.port = port_override;
    Server srv(cfg);
    g_server = &srv;
    if (!srv.start()) return 1;
    srv.run();
    return 0;
}
