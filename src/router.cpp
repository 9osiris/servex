// exact and prefix route matching
#include "router.h"

void Router::add_exact(const std::string& path, Handler h) {
    exact_[path] = h;
}

void Router::add_prefix(const std::string& prefix, Handler h) {
    prefixes_.push_back({prefix, h});
}

Handler Router::find(const std::string& path) const {
    auto it = exact_.find(path);
    if (it != exact_.end()) return it->second;
    const Handler* best = nullptr;
    size_t best_len = 0;
    for (auto& p : prefixes_) {
        if (path.compare(0, p.first.size(), p.first) == 0 &&
            p.first.size() > best_len) {
            best = &p.second;
            best_len = p.first.size();
        }
    }
    return best ? *best : Handler();
}

std::string Router::match_label(const std::string& path) const {
    auto it = exact_.find(path);
    if (it != exact_.end()) return path;
    std::string best;
    for (auto& p : prefixes_) {
        if (path.compare(0, p.first.size(), p.first) == 0 &&
            p.first.size() > best.size())
            best = p.first;
    }
    return best; // "" when nothing matched
}
