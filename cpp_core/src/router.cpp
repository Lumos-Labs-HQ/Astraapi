#include "router.hpp"
#include "compat.hpp"
#include <algorithm>
#include <cstring>

// ── Hono.js-inspired radix trie router ──────────────────────────────────────
// Phase A: Static routes → O(1) hash map (transparent string_view lookup)
// Phase B: Parametric routes → radix trie with first-byte dispatch
// Supports: /static, /{param}, /{param:path}
// Zero-allocation matching: string_view into request buffer + trie nodes

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

    // Static segment: find next param/end boundary first
    size_t end = pos;
    while (end < path.size()) {
        if (end > pos && path[end] == '/' && end + 1 < path.size() && path[end + 1] == '{') {
            break;
        }
        end++;
    }
    std::string new_seg = path.substr(pos, end - pos);

    // Try first-byte dispatch to find existing child
    unsigned char fb = (unsigned char)path[pos];
    if (fb < 128) {
        int16_t idx = node.dispatch[fb];
        if (idx >= 0) {
            auto& child = node.children[idx];
            if (child.param_name.empty() && !child.prefix.empty()) {
                // Full prefix match — recurse into child
                if (new_seg.size() >= child.prefix.size() &&
                    new_seg.compare(0, child.prefix.size(), child.prefix) == 0) {
                    return insert_recursive(child, path, index, pos + child.prefix.size());
                }

                // Partial match — split the node (radix trie prefix splitting)
                size_t common_len = 0;
                size_t max_common = std::min(child.prefix.size(), new_seg.size());
                while (common_len < max_common &&
                       child.prefix[common_len] == new_seg[common_len]) {
                    common_len++;
                }

                if (common_len > 0) {
                    // Create intermediate node with the common prefix
                    Node intermediate;
                    intermediate.prefix = child.prefix.substr(0, common_len);

                    // Shorten the existing child's prefix to remainder after common
                    child.prefix = child.prefix.substr(common_len);

                    // Move existing child under intermediate
                    intermediate.children.push_back(std::move(child));
                    // Update intermediate dispatch for the moved child's new first byte
                    if (!intermediate.children[0].prefix.empty()) {
                        unsigned char ofb = (unsigned char)intermediate.children[0].prefix[0];
                        if (ofb < 128) intermediate.dispatch[ofb] = 0;
                    }
                    // Transfer param_child/catch_all_child to intermediate
                    // (they belonged to the old child's NODE, which is now under intermediate)
                    // The old child itself keeps its own param_child/catch_all_child.
                    // The intermediate is a new branching point — no params of its own.

                    // Replace original child slot with intermediate
                    node.children[idx] = std::move(intermediate);

                    // Recurse into intermediate to continue insertion
                    return insert_recursive(node.children[idx], path, index, pos + common_len);
                }
            }
        }
    }

    // No matching child — create new child with the segment prefix
    Node child;
    child.prefix = std::move(new_seg);
    node.children.push_back(std::move(child));
    int16_t child_idx = (int16_t)(node.children.size() - 1);
    if (fb < 128) {
        // Track dispatch collisions for match-time optimization
        if (node.dispatch[fb] >= 0 && node.dispatch[fb] != child_idx) {
            node.has_dispatch_collision = true;
        }
        node.dispatch[fb] = child_idx;
    }
    return insert_recursive(node.children.back(), path, index, end);
}

std::optional<MatchParams> Router::at(const char* path, size_t len) const {
    // Phase A: Try static hash map first — O(1), zero allocation (transparent lookup)
    std::string_view path_sv(path, len);
    auto it = static_routes_.find(path_sv);
    if (it != static_routes_.end()) {
        MatchParams m;
        m.route_index = it->second;
        return m;
    }

    // Phase B: Recursive trie match — zero allocation, with backtracking
    MatchParams result;
    if (match_recursive(root_, path, len, 0, result)) {
        return result;
    }
    return std::nullopt;
}

bool Router::match_recursive(
    const Node& node, const char* path, size_t len,
    size_t pos, MatchParams& result) const
{
    if (pos >= len) {
        if (node.route_index >= 0) {
            result.route_index = node.route_index;
            return true;
        }
        // Check children with empty prefix (terminal)
        for (const auto& child : node.children) {
            if (child.prefix.empty() && child.route_index >= 0) {
                result.route_index = child.route_index;
                return true;
            }
        }
        return false;
    }

    // 1) First-byte dispatch for static child
    unsigned char fb = (unsigned char)path[pos];
    int16_t tried_idx = -1;
    if (LIKELY(fb < 128)) {
        int16_t idx = node.dispatch[fb];
        if (LIKELY(idx >= 0)) {
            tried_idx = idx;
            const auto& child = node.children[idx];
            size_t plen = child.prefix.size();
            if (child.param_name.empty() && plen > 0 &&
                pos + plen <= len && memcmp(path + pos, child.prefix.c_str(), plen) == 0) {
                if (match_recursive(child, path, len, pos + plen, result))
                    return true;
            }
        }
    }

    // 1b) Fallback: scan all static children — only needed when dispatch has collisions
    // Most route trees are collision-free, making this scan unreachable.
    if (UNLIKELY(node.has_dispatch_collision)) {
        for (int16_t ci = 0; ci < (int16_t)node.children.size(); ci++) {
            if (ci == tried_idx) continue;
            const auto& child = node.children[ci];
            if (child.param_name.empty() && !child.prefix.empty()) {
                size_t plen = child.prefix.size();
                if (pos + plen <= len && memcmp(path + pos, child.prefix.c_str(), plen) == 0) {
                    if (match_recursive(child, path, len, pos + plen, result))
                        return true;
                }
            }
        }
    }

    // 2) Param child — consume /{value}
    if (node.param_child >= 0 && path[pos] == '/') {
        const auto& child = node.children[node.param_child];
        size_t s = pos + 1, e = s;
        while (e < len && path[e] != '/') e++;
        if (e > s) {
            int prev_count = result.param_count;
            result.add(std::string_view(child.param_name),
                       std::string_view(path + s, e - s));
            if (match_recursive(child, path, len, e, result))
                return true;
            result.param_count = prev_count;  // backtrack
        }
    }

    // 3) Catch-all child — consume rest
    if (node.catch_all_child >= 0 && path[pos] == '/') {
        const auto& child = node.children[node.catch_all_child];
        size_t s = pos + 1;
        result.add(std::string_view(child.param_name),
                   std::string_view(path + s, len - s));
        result.route_index = child.route_index;
        return child.route_index >= 0;
    }

    return false;
}
