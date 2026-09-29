// middleware chain: regex rewrites, redirects, basic auth,
// and custom response headers
#include "middleware.h"
#include "crypto.h"
#include "util.h"
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sys/stat.h>

// compiled rewrite regexes, cached per pattern
struct RegexCache {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<std::regex>> cache;

    std::shared_ptr<std::regex> get(const std::string& pattern) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(pattern);
        if (it != cache.end()) return it->second;
        std::shared_ptr<std::regex> re;
        try {
            re = std::make_shared<std::regex>(pattern);
        } catch (...) {
            re = std::make_shared<std::regex>("a^"); // never matches
        }
        cache[pattern] = re;
        return re;
    }
};

static RegexCache& regex_cache() {
    static RegexCache c;
    return c;
}

// parsed htpasswd-style credentials file, reloaded when it changes
struct AuthFile {
    std::mutex mutex;
    std::string path;
    long mtime = 0;
    std::map<std::string, std::string> users; // user -> stored credential

    bool refresh() {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (st.st_mtime == mtime && !users.empty()) return true;
        std::map<std::string, std::string> fresh;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            fresh[trim(line.substr(0, c))] = trim(line.substr(c + 1));
        }
        users = fresh;
        mtime = st.st_mtime;
        return true;
    }

    bool check(const std::string& user, const std::string& pass) {
        if (!refresh()) return false;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = users.find(user);
        if (it == users.end()) return false;
        const std::string& stored = it->second;
        if (starts_with(stored, "{SHA}")) {
            // {SHA}base64(sha1(password)), like classic htpasswd -s
            std::string want = base64_encode(sha1_raw(pass));
            if (want.size() != stored.size() - 5) return false;
            int diff = 0;
            for (size_t i = 0; i < want.size(); i++)
                diff |= want[i] ^ stored[5 + i];
            return diff == 0;
        }
        return stored == pass;
    }
};

static std::mutex g_auth_mutex;
static std::map<std::string, std::shared_ptr<AuthFile>> g_auth_files;

static std::shared_ptr<AuthFile> auth_file_for(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    auto it = g_auth_files.find(path);
    if (it != g_auth_files.end()) return it->second;
    auto af = std::make_shared<AuthFile>();
    af->path = path;
    g_auth_files[path] = af;
    return af;
}

static bool check_basic_auth(const LocationConfig* loc,
                             const HttpRequest& req) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return false;
    std::string v = trim(it->second);
    if (v.size() < 6 || lower(v.substr(0, 6)) != "basic ") return false;
    std::string decoded;
    if (!base64_decode(trim(v.substr(6)), decoded)) return false;
    size_t c = decoded.find(':');
    if (c == std::string::npos) return false;
    return auth_file_for(loc->auth_file)
        ->check(decoded.substr(0, c), decoded.substr(c + 1));
}

static void apply_add_headers(const LocationConfig* loc, HttpResponse& res) {
    for (auto& ah : loc->add_headers) res.headers[ah.first] = ah.second;
}

MiddlewareAction apply_middleware(const LocationConfig* loc, HttpRequest& req) {
    MiddlewareAction act;
    if (!loc) return act;

    // 1. internal rewrite: change the path, then re-resolve the location
    if (!loc->rewrite_pattern.empty()) {
        auto re = regex_cache().get(loc->rewrite_pattern);
        if (std::regex_search(req.path, *re)) {
            std::string np;
            try {
                np = std::regex_replace(req.path, *re,
                                        loc->rewrite_replacement);
            } catch (...) {
                np = req.path;
            }
            if (np.size() > 1 && np[0] == '/' && np != req.path) {
                req.path = np;
                act.rewritten = true;
                return act;
            }
        }
    }

    // 2. redirect: external, ends request handling
    if (!loc->redirect_to.empty()) {
        act.stop = true;
        act.res.status = loc->redirect_code;
        act.res.reason = reason_phrase(act.res.status);
        std::string target = loc->redirect_to;
        if (!req.query.empty() && target.find('?') == std::string::npos)
            target += "?" + req.query;
        act.res.headers["Location"] = target;
        act.res.headers["Content-Type"] = "text/plain";
        act.res.body = "redirecting to " + target + "\n";
        apply_add_headers(loc, act.res);
        return act;
    }

    // 3. basic auth
    if (!loc->auth_realm.empty()) {
        if (!check_basic_auth(loc, req)) {
            act.stop = true;
            act.res = make_error(401, "authentication is required");
            act.res.headers["WWW-Authenticate"] =
                "Basic realm=\"" + loc->auth_realm + "\"";
            apply_add_headers(loc, act.res);
            return act;
        }
    }
    return act;
}
