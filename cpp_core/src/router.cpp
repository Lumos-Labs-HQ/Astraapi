#include "router.hpp"
#include <algorithm>
#include <cstring>

// ── Hono.js-inspired radix trie router ──────────────────────────────────────
// Phase A: Static routes → O(1) hash map
// Phase B: Parametric routes → radix trie with first-byte dispatch
// Supports: /static, /{param}, /{param:path}

bool Router::insert(const std::string& pattern, int index) {
    // Phase A: Routes with no parameters go into O(1) hash map
    if (pattern.find('{') == std::string::npos) {
        static_routes_[pattern] = index;
        return true;
    }
    // Phase B: Parametric routes go into the trie
    return insert_recursive(root_, pattern, index, 0);
}

bool Router::insert_recursive(Node& node, const std::string& path, int index, size_t pos) {
    if (pos >= path.size()) {
        node.route_index = index;
        return true;
    }

    // Check for parameter segment: /{name} or /{name:path}
    if (path[pos] == '/' && pos + 1 < path.size() && path[pos + 1] == '{') {
        size_t close = path.find('}', pos + 2);
        if (close == std::string::npos) return false;

        std::string param_content = path.substr(pos + 2, close - pos - 2);
        std::string param_name;
        bool is_catch_all = false;

        size_t colon = param_content.find(':');
        if (colon != std::string::npos) {
            param_name = param_content.substr(0, colon);
            std::string constraint = param_content.substr(colon + 1);
            if (constraint == "path") {
                is_catch_all = true;
            }
        } else {
            param_name = param_content;
        }

        // Find existing param child via indexed lookup
        if (!is_catch_all && node.param_child >= 0) {
            auto& child = node.children[node.param_child];
            if (child.param_name == param_name) {
                return insert_recursive(child, path, index, close + 1);
            }
        }
        if (is_catch_all && node.catch_all_child >= 0) {
            auto& child = node.children[node.catch_all_child];
            if (child.param_name == param_name) {
                return insert_recursive(child, path, index, close + 1);
            }
        }

        // Create new param child
        Node child;
        child.prefix = "/";
        child.param_name = param_name;
        child.is_catch_all = is_catch_all;
        node.children.push_back(std::move(child));
        int16_t child_idx = (int16_t)(node.children.size() - 1);
        if (is_catch_all) {
            node.catch_all_child = child_idx;
        } else {
            node.param_child = child_idx;
        }
        return insert_recursive(node.children.back(), path, index, close + 1);
    }

    // Static segment: find matching child via first-byte dispatch
    unsigned char fb = (unsigned char)path[pos];
    if (fb < 128) {
        int16_t idx = node.dispatch[fb];
        if (idx >= 0) {
            auto& child = node.children[idx];
            if (child.param_name.empty() && !child.prefix.empty() &&
                path.compare(pos, child.prefix.size(), child.prefix) == 0) {
                return insert_recursive(child, path, index, pos + child.prefix.size());
            }
        }
    }

    // No matching child — find next param/end boundary
    size_t end = pos;
    while (end < path.size()) {
        if (end > pos && path[end] == '/' && end + 1 < path.size() && path[end + 1] == '{') {
            break;
        }
        end++;
    }

    Node child;
    child.prefix = path.substr(pos, end - pos);
    node.children.push_back(std::move(child));
    int16_t child_idx = (int16_t)(node.children.size() - 1);
    // Update dispatch table for first byte of prefix
    if (fb < 128) {
        node.dispatch[fb] = child_idx;
    }
    return insert_recursive(node.children.back(), path, index, end);
}

std::optional<MatchParams> Router::at(const char* path, size_t len) const {
    // Phase A: Try static hash map first — O(1)
    std::string path_str(path, len);
    auto it = static_routes_.find(path_str);
    if (it != static_routes_.end()) {
        return MatchParams{it->second, {}};
    }

    // Phase B: Fall back to trie for parametric routes
    std::vector<std::pair<std::string, std::string>> params;
    params.reserve(4);
    return match_recursive(root_, path, len, 0, params);
}

std::optional<MatchParams> Router::match_recursive(
    const Node& node, const char* path, size_t len, size_t pos,
    std::vector<std::pair<std::string, std::string>>& params
) const {
    if (pos >= len) {
        if (node.route_index >= 0) {
            return MatchParams{node.route_index, params};
        }
        // Check children for empty-prefix terminal
        for (const auto& child : node.children) {
            if (child.prefix.empty() && child.route_index >= 0) {
                return MatchParams{child.route_index, params};
            }
        }
        return std::nullopt;
    }

    // First-byte dispatch for static children — O(1) lookup
    unsigned char fb = (unsigned char)path[pos];
    int16_t dispatch_idx = (fb < 128) ? node.dispatch[fb] : -1;
    if (dispatch_idx >= 0) {
        const auto& child = node.children[dispatch_idx];
        size_t plen = child.prefix.size();
        if (pos + plen <= len && memcmp(path + pos, child.prefix.c_str(), plen) == 0) {
            auto result = match_recursive(child, path, len, pos + plen, params);
            if (result) return result;
        }
    }

    // Fallback: scan all static children (handles dispatch table collision
    // when multiple parametric route prefixes share the same first byte)
    for (size_t i = 0; i < node.children.size(); i++) {
        if ((int16_t)i == dispatch_idx) continue;   // already tried above
        const auto& child = node.children[i];
        if (!child.param_name.empty()) continue;    // skip param/catch-all nodes
        size_t plen = child.prefix.size();
        if (plen > 0 && pos + plen <= len &&
            memcmp(path + pos, child.prefix.c_str(), plen) == 0) {
            auto result = match_recursive(child, path, len, pos + plen, params);
            if (result) return result;
        }
    }

    // Parameter child — consume until next '/'
    if (node.param_child >= 0) {
        const auto& child = node.children[node.param_child];
        if (pos < len && path[pos] == '/') {
            size_t start = pos + 1;
            size_t end = start;
            while (end < len && path[end] != '/') end++;

            if (end > start) {
                params.push_back({child.param_name, std::string(path + start, end - start)});
                auto result = match_recursive(child, path, len, end, params);
                if (result) return result;
                params.pop_back();
            }
        }
    }

    // Catch-all child — consume rest of path
    if (node.catch_all_child >= 0) {
        const auto& child = node.children[node.catch_all_child];
        if (pos < len && path[pos] == '/') {
            size_t start = pos + 1;
            if (start <= len) {
                params.push_back({child.param_name, std::string(path + start, len - start)});
                if (child.route_index >= 0) {
                    return MatchParams{child.route_index, params};
                }
                auto result = match_recursive(child, path, len, len, params);
                if (result) return result;
                params.pop_back();
            }
        }
    }

    return std::nullopt;
}
