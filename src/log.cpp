// apache combined-ish access logging
#include "log.h"
#include "http.h"
#include "server.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

// shift access.log -> access.log.1 -> access.log.2 ..., dropping the oldest
static void rotate_log(const std::string& path, int archives) {
    if (archives < 1) archives = 1;
    remove((path + "." + std::to_string(archives)).c_str());
    for (int i = archives - 1; i >= 1; i--)
        rename((path + "." + std::to_string(i)).c_str(),
               (path + "." + std::to_string(i + 1)).c_str());
    rename(path.c_str(), (path + ".1").c_str());
}

void log_access(ServerContext& ctx, const std::string& log_path,
                const std::string& client_ip, const HttpRequest& req,
                int status, long bytes_sent) {
    char tbuf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);
    std::strftime(tbuf, sizeof(tbuf), "%d/%b/%Y:%H:%M:%S %z", &tm);
    std::string referer = "-";
    std::string agent = "-";
    auto it = req.headers.find("referer");
    if (it != req.headers.end()) referer = it->second;
    it = req.headers.find("user-agent");
    if (it != req.headers.end()) agent = it->second;
    std::string line = client_ip + " - - [" + tbuf + "] \"" +
        req.method + " " + req.target + " " + req.version + "\" " +
        std::to_string(status) + " " + std::to_string(bytes_sent) +
        " \"" + referer + "\" \"" + agent + "\"\n";
    std::lock_guard<std::mutex> lock(ctx.log_mutex);
    if (log_path.empty()) {
        std::cout << line << std::flush;
    } else {
        std::ofstream f(log_path, std::ios::app);
        f << line;
        f.close();
        if (ctx.log_max_bytes > 0) {
            struct stat st;
            if (stat(log_path.c_str(), &st) == 0 &&
                st.st_size >= ctx.log_max_bytes)
                rotate_log(log_path, ctx.log_archives);
        }
    }
}
