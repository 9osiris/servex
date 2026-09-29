// cgi/1.1 execution: fork/exec scripts, pass env vars, parse headers
#include "cgi.h"
#include "config.h"
#include "server.h"
#include "util.h"
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static const long MAX_CGI_BODY = 8 * 1024 * 1024;
static const int CGI_TIMEOUT_SEC = 5;

static long long now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// join docroot + path safely; "" when the path escapes the root
static std::string join_safe(const std::string& docroot,
                             const std::string& req_path) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= req_path.size(); i++) {
        char c = i < req_path.size() ? req_path[i] : '/';
        if (c == '/') {
            if (cur == "..") {
                if (parts.empty()) return "";
                parts.pop_back();
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string full = docroot;
    for (auto& p : parts) full += "/" + p;
    return full;
}

// find the script: longest leading path that is an executable file.
// rest becomes path_info. "" when nothing executable matches.
static std::string find_script(const std::string& docroot,
                               const std::string& req_path,
                               std::string& path_info) {
    std::string cur;
    std::string best;
    std::string best_rest;
    for (size_t i = 0; i <= req_path.size(); i++) {
        char c = i < req_path.size() ? req_path[i] : '/';
        if (c == '/') {
            std::string full = join_safe(docroot, cur.empty() ? "/" : cur);
            struct stat st;
            if (!full.empty() && stat(full.c_str(), &st) == 0 &&
                S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) {
                best = full;
                best_rest = req_path.substr(cur.size());
            }
        }
        cur += c;
    }
    path_info = best_rest;
    return best;
}

static std::string header_env_name(const std::string& name) {
    std::string out = "HTTP_";
    for (char c : name) {
        if (c == '-') out += '_';
        else out += toupper((unsigned char)c);
    }
    return out;
}

static void set_env(const std::string& k, const std::string& v) {
    setenv(k.c_str(), v.c_str(), 1);
}

// one select-driven pump: feed stdin, drain stdout/stderr, all under a
// single deadline. returns the child wait status, or -1 on timeout.
static int pump_cgi(pid_t pid, int stdin_fd, int stdout_fd, int stderr_fd,
                    const std::string& body, std::string& output,
                    std::string& errors) {
    long long deadline = now_ns() + (long long)CGI_TIMEOUT_SEC * 1000000000LL;
    for (int fd : {stdin_fd, stdout_fd, stderr_fd}) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    size_t body_pos = 0;
    bool stdin_open = true, stdout_open = true, stderr_open = true;
    if (body.empty()) {
        close(stdin_fd); // nothing to feed, give the child eof now
        stdin_open = false;
    }
    bool child_done = false;
    int child_status = 0;
    char buf[8192];
    while (stdin_open || stdout_open || stderr_open || !child_done) {
        long long remain = deadline - now_ns();
        if (remain <= 0) {
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
            return -1;
        }
        int status = 0;
        if (!child_done && waitpid(pid, &status, WNOHANG) == pid) {
            child_done = true;
            child_status = status;
        }
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        int maxfd = -1;
        if (stdin_open && body_pos < body.size()) {
            FD_SET(stdin_fd, &wset);
            maxfd = stdin_fd;
        }
        if (stdout_open) {
            FD_SET(stdout_fd, &rset);
            if (stdout_fd > maxfd) maxfd = stdout_fd;
        }
        if (stderr_open) {
            FD_SET(stderr_fd, &rset);
            if (stderr_fd > maxfd) maxfd = stderr_fd;
        }
        if (maxfd < 0) {
            if (child_done) break;
            usleep(10000);
            continue;
        }
        struct timeval tv;
        tv.tv_sec = (long)(remain / 1000000000LL);
        tv.tv_usec = (long)((remain % 1000000000LL) / 1000);
        int rc = select(maxfd + 1, &rset, &wset, nullptr, &tv);
        if (rc < 0) continue;
        if (rc == 0) continue; // loop back to the deadline check
        if (stdin_open && FD_ISSET(stdin_fd, &wset)) {
            ssize_t r = write(stdin_fd, body.data() + body_pos,
                              body.size() - body_pos);
            if (r > 0) body_pos += r;
            if (r < 0 && errno != EAGAIN) stdin_open = false;
            if (body_pos >= body.size()) {
                close(stdin_fd);
                stdin_open = false; // eof to the child
            }
        }
        for (int i = 0; i < 2; i++) {
            int fd = i == 0 ? stdout_fd : stderr_fd;
            bool* open = i == 0 ? &stdout_open : &stderr_open;
            std::string* dst = i == 0 ? &output : &errors;
            if (!*open || !FD_ISSET(fd, &rset)) continue;
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r > 0) {
                if ((long)dst->size() + r > MAX_CGI_BODY) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    return -2; // output cap exceeded
                }
                dst->append(buf, r);
            } else if (r == 0) {
                *open = false; // eof
            }
        }
    }
    return child_status;
}

// parse the script's header block; fills res headers/status/body.
// false when the output is not a valid cgi response.
static bool parse_cgi_output(const std::string& out, HttpResponse& res) {
    size_t end = out.find("\r\n\r\n");
    size_t sep_len = 4;
    if (end == std::string::npos) {
        end = out.find("\n\n");
        sep_len = 2;
    }
    if (end == std::string::npos) return false;
    std::string head = out.substr(0, end);
    res.body = out.substr(end + sep_len);
    int status = 200;
    bool has_status = false;
    for (auto& line : split(head, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string name = lower(trim(line.substr(0, c)));
        std::string value = trim(line.substr(c + 1));
        if (name == "status") {
            long code;
            std::vector<std::string> parts = split_ws(value);
            if (!parts.empty() && parse_long(parts[0], code) && code >= 100 &&
                code < 600)
                status = (int)code;
            has_status = true;
        } else if (name == "location" && !has_status) {
            res.headers["Location"] = value;
            if (status == 200) status = 302;
        } else {
            res.headers[name] = value;
        }
    }
    res.status = status;
    res.reason = reason_phrase(status);
    if (res.headers.count("content-type") == 0)
        res.headers["Content-Type"] = "text/html";
    return true;
}

HttpResponse run_cgi(const HttpRequest& req, ServerContext& ctx,
                     const LocationConfig& loc, const std::string& docroot,
                     const std::string& client_ip) {
    (void)loc;
    ctx.stats.cgi_runs++;
    std::string path_info;
    std::string script = find_script(docroot, req.path, path_info);
    if (script.empty()) {
        std::string probe = join_safe(docroot, req.path);
        struct stat st;
        if (probe.empty() || stat(probe.c_str(), &st) != 0)
            return make_error(404, "no such cgi script: " + req.path);
        return make_error(403, "cgi script is not executable");
    }

    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0)
        return make_error(500, "could not create cgi pipes");

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return make_error(500, "could not fork for cgi");
    }
    if (pid == 0) {
        // child: wire up stdio, set the cgi environment, exec the script
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        size_t slash = script.rfind('/');
        std::string dir =
            slash == std::string::npos ? "." : script.substr(0, slash);
        if (chdir(dir.c_str()) != 0) _exit(127);

        set_env("GATEWAY_INTERFACE", "CGI/1.1");
        set_env("SERVER_PROTOCOL", "HTTP/1.1");
        set_env("SERVER_SOFTWARE", "servex");
        set_env("REQUEST_METHOD", req.method);
        set_env("QUERY_STRING", req.query);
        set_env("SCRIPT_NAME", req.path.substr(0, req.path.size() -
                                                         path_info.size()));
        set_env("PATH_INFO", path_info);
        set_env("DOCUMENT_ROOT", docroot);
        set_env("REMOTE_ADDR", client_ip);
        auto host_it = req.headers.find("host");
        std::string host = host_it == req.headers.end() ? "" : host_it->second;
        size_t colon = host.rfind(':');
        set_env("SERVER_NAME",
                colon == std::string::npos ? host : host.substr(0, colon));
        set_env("SERVER_PORT",
                colon == std::string::npos ? "80" : host.substr(colon + 1));
        auto ct = req.headers.find("content-type");
        if (ct != req.headers.end()) set_env("CONTENT_TYPE", ct->second);
        auto cl = req.headers.find("content-length");
        set_env("CONTENT_LENGTH",
                cl != req.headers.end() ? cl->second
                                        : std::to_string(req.body.size()));
        for (auto& kv : req.headers) {
            if (kv.first == "content-type" || kv.first == "content-length")
                continue;
            set_env(header_env_name(kv.first), kv.second);
        }
        std::string name =
            slash == std::string::npos ? script : script.substr(slash + 1);
        execl(script.c_str(), name.c_str(), (char*)nullptr);
        _exit(127); // exec failed
    }

    // parent: pump stdio under one deadline, then interpret the output
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    std::string output, errors;
    int status = pump_cgi(pid, stdin_pipe[1], stdout_pipe[0], stderr_pipe[0],
                          req.body, output, errors);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    if (!errors.empty())
        std::cerr << "servex cgi " << script << " stderr: " << errors;
    if (status == -1)
        return make_error(504, "cgi script timed out");
    if (status == -2)
        return make_error(500, "cgi output too large");

    HttpResponse res;
    if (!parse_cgi_output(output, res))
        return make_error(500, "cgi script returned no valid headers");
    res.chunked = true; // stream the script output to the client
    return res;
}
