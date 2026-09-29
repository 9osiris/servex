// block-structured config parsing: server/location blocks,
// include directives, and ${variable} expansion
#include "config.h"
#include "util.h"
#include <fstream>
#include <glob.h>
#include <iostream>
#include <sstream>

static bool parse_bool(const std::string& s) {
    std::string v = lower(s);
    return v == "on" || v == "true" || v == "yes" || v == "1";
}

// strip a # comment, honoring double quotes
static std::string strip_comment(const std::string& line) {
    std::string out;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\'))
            in_quote = !in_quote;
        if (c == '#' && !in_quote) break;
        out += c;
    }
    return out;
}

// remove one pair of surrounding double quotes
static std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); i++) {
            if (v[i] == '\\' && i + 2 < v.size()) {
                i++;
                out += v[i];
            } else {
                out += v[i];
            }
        }
        return out;
    }
    return v;
}

static std::string expand_one(const std::string& v,
                              const std::map<std::string, std::string>& vars) {
    std::string out;
    for (size_t i = 0; i < v.size();) {
        if (v[i] == '$' && i + 1 < v.size() && v[i + 1] == '{') {
            size_t e = v.find('}', i + 2);
            if (e == std::string::npos) {
                out += v.substr(i);
                break;
            }
            std::string name = v.substr(i + 2, e - i - 2);
            auto it = vars.find(name);
            if (it != vars.end()) out += it->second; // undefined -> empty
            i = e + 1;
        } else {
            out += v[i++];
        }
    }
    return out;
}

static std::string dirname_of(const std::string& path) {
    size_t s = path.rfind('/');
    return s == std::string::npos ? "." : path.substr(0, s);
}

struct Parser {
    Config cfg;
    // legacy top-level keys, used to seed server blocks
    std::string legacy_docroot = "./www";
    bool legacy_dir_listing = true;
    std::string legacy_log;

    struct Frame {
        int kind = 0; // 1 = server, 2 = location
        ServerBlock server;
        LocationConfig loc;
    };
    std::vector<Frame> stack;

    void error(const std::string& where, const std::string& msg) {
        cfg.errors.push_back(where + ": " + msg);
        std::cerr << "servex config: " << where << ": " << msg << std::endl;
    }

    void apply_top_key(const std::string& key, const std::string& val,
                       const std::string& where) {
        try {
            if (key == "port") cfg.port = std::stoi(val);
            else if (key == "workers") cfg.workers = std::stoi(val);
            else if (key == "log") {
                cfg.log_path = val;
                legacy_log = val;
            } else if (key == "log_max_bytes") {
                cfg.log_max_bytes = std::stol(val);
            } else if (key == "log_archives") {
                cfg.log_archives = std::stoi(val);
            } else if (key == "docroot") {
                legacy_docroot = val;
            } else if (key == "dir_listing") {
                legacy_dir_listing = parse_bool(val);
            } else {
                error(where, "unknown top-level key '" + key + "'");
            }
        } catch (...) {
            error(where, "bad value for '" + key + "'");
        }
    }

    void apply_server_key(ServerBlock& s, const std::string& key,
                          const std::string& val, const std::string& where) {
        try {
            if (key == "host") s.hosts = split_ws(val);
            else if (key == "docroot") s.docroot = val;
            else if (key == "dir_listing") s.dir_listing = parse_bool(val);
            else if (key == "log") s.log_path = val;
            else if (key == "rate") s.rate = std::stod(val);
            else if (key == "burst") s.burst = std::stol(val);
            else error(where, "unknown server key '" + key + "'");
        } catch (...) {
            error(where, "bad value for '" + key + "'");
        }
    }

    void apply_location_key(LocationConfig& l, const std::string& key,
                            const std::string& val,
                            const std::string& where) {
        try {
            if (key == "proxy_pass") l.proxy_pass = val;
            else if (key == "cgi") l.cgi = parse_bool(val);
            else if (key == "websocket") l.websocket = parse_bool(val);
            else if (key == "auth_realm") l.auth_realm = val;
            else if (key == "auth_file") l.auth_file = val;
            else if (key == "add_header") {
                size_t c = val.find(':');
                if (c == std::string::npos) {
                    error(where, "add_header needs 'Name: value'");
                } else {
                    l.add_headers.push_back(
                        {trim(val.substr(0, c)), trim(val.substr(c + 1))});
                }
            } else if (key == "rewrite") {
                size_t sp = val.find_first_of(" \t");
                if (sp == std::string::npos) {
                    error(where, "rewrite needs 'pattern replacement'");
                } else {
                    l.rewrite_pattern = trim(val.substr(0, sp));
                    l.rewrite_replacement = trim(val.substr(sp + 1));
                }
            } else if (key == "redirect") {
                l.redirect_to = val;
            } else if (key == "redirect_code") {
                l.redirect_code = std::stoi(val);
            } else if (key == "rate") {
                l.rate = std::stod(val);
            } else if (key == "burst") {
                l.burst = std::stol(val);
            } else {
                error(where, "unknown location key '" + key + "'");
            }
        } catch (...) {
            error(where, "bad value for '" + key + "'");
        }
    }

    void parse_file(const std::string& path, int depth) {
        if (depth > 8) {
            error(path, "include depth exceeded");
            return;
        }
        std::ifstream f(path);
        if (!f) {
            error(path, "cannot open config file");
            return;
        }
        std::string dir = dirname_of(path);
        std::string raw;
        int lineno = 0;
        while (std::getline(f, raw)) {
            lineno++;
            std::string where = path + ":" + std::to_string(lineno);
            std::string line = trim(strip_comment(raw));
            if (line.empty()) continue;

            if (line == "}") {
                if (stack.empty()) {
                    error(where, "stray closing brace");
                    continue;
                }
                Frame fr = stack.back();
                stack.pop_back();
                if (fr.kind == 2) {
                    if (stack.empty() || stack.back().kind != 1) {
                        error(where, "location block outside a server");
                        continue;
                    }
                    stack.back().server.locations.push_back(fr.loc);
                } else {
                    cfg.servers.push_back(fr.server);
                }
                continue;
            }

            std::vector<std::string> toks = split_ws(line);
            if (toks[0] == "include" && stack.empty()) {
                std::string pattern = unquote(line.substr(7));
                if (!pattern.empty() && pattern[0] != '/')
                    pattern = dir + "/" + pattern;
                glob_t g;
                int rc = glob(pattern.c_str(), 0, nullptr, &g);
                if (rc == 0) {
                    for (size_t i = 0; i < g.gl_pathc; i++)
                        parse_file(g.gl_pathv[i], depth + 1);
                } else if (rc == GLOB_NOMATCH) {
                    error(where, "include matched nothing: " + pattern);
                }
                globfree(&g);
                continue;
            }
            if (toks[0] == "set" && stack.empty()) {
                if (toks.size() < 3) {
                    error(where, "set needs a name and a value");
                    continue;
                }
                std::string name = toks[1];
                if (!name.empty() && name[0] == '$') name = name.substr(1);
                size_t vp = line.find(toks[2]);
                cfg.vars[name] = unquote(line.substr(vp));
                continue;
            }
            if (toks[0] == "server" && stack.empty()) {
                if (toks.size() != 2 || toks[1] != "{") {
                    error(where, "expected 'server {'");
                    continue;
                }
                Frame fr;
                fr.kind = 1;
                fr.server.docroot = legacy_docroot;
                fr.server.dir_listing = legacy_dir_listing;
                fr.server.log_path = legacy_log;
                stack.push_back(fr);
                continue;
            }
            if (toks[0] == "location" && !stack.empty() &&
                stack.back().kind == 1) {
                if (toks.size() != 3 || toks[2] != "{") {
                    error(where, "expected 'location <prefix> {'");
                    continue;
                }
                Frame fr;
                fr.kind = 2;
                fr.loc.match = toks[1];
                stack.push_back(fr);
                continue;
            }
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(line.substr(0, eq));
                std::string val = unquote(line.substr(eq + 1));
                if (stack.empty()) {
                    apply_top_key(key, val, where);
                } else if (stack.back().kind == 1) {
                    apply_server_key(stack.back().server, key, val, where);
                } else {
                    apply_location_key(stack.back().loc, key, val, where);
                }
                continue;
            }
            error(where, "cannot parse line: " + line);
        }
    }

    // expand ${vars} in every string field after all files are read
    void expand_all() {
        for (int pass = 0; pass < 5; pass++) {
            bool changed = false;
            auto ex = [&](std::string& s) {
                std::string n = expand_one(s, cfg.vars);
                if (n != s) {
                    s = n;
                    changed = true;
                }
            };
            ex(cfg.log_path);
            for (auto& sv : cfg.servers) {
                ex(sv.docroot);
                ex(sv.log_path);
                for (auto& h : sv.hosts) ex(h);
                for (auto& lc : sv.locations) {
                    ex(lc.proxy_pass);
                    ex(lc.auth_realm);
                    ex(lc.auth_file);
                    ex(lc.rewrite_pattern);
                    ex(lc.rewrite_replacement);
                    ex(lc.redirect_to);
                    for (auto& ah : lc.add_headers) {
                        ex(ah.first);
                        ex(ah.second);
                    }
                }
            }
            if (!changed) break;
        }
    }
};

Config load_config(const std::string& path) {
    Parser p;
    p.parse_file(path, 0);
    if (!p.stack.empty())
        p.error(path, "unclosed block at end of file");
    p.expand_all();
    if (p.cfg.servers.empty()) {
        // no server blocks: legacy flat config becomes one default server
        ServerBlock s;
        s.docroot = p.legacy_docroot;
        s.dir_listing = p.legacy_dir_listing;
        s.log_path = p.legacy_log;
        p.cfg.servers.push_back(s);
    }
    return p.cfg;
}
