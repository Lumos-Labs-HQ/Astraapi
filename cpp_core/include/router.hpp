#pragma once
#include <string>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <unordered_map>
#include <array>
#include <cstdint>

// Radix trie router for URL pattern matching.
// Supports: /static, /{param}, /{param:path} patterns.
// Thread-safe: shared_mutex for concurrent reads.
//
// Hono.js-inspired two-phase lookup:
//   Phase A: Static route hash map — O(1) for exact matches (no params)
//   Phase B: Radix trie with first-byte dispatch — O(k) for parametric routes

struct MatchParams {
    int route_index;
    std::vector<std::pair<std::string, std::string>> params;
};

class Router {
public:
    Router() = default;

    // Insert a route pattern. Returns true on success.
    bool insert(const std::string& pattern, int index);

    // Match a path against registered routes. Returns nullopt if no match.
    std::optional<MatchParams> at(const char* path, size_t len) const;
    std::optional<MatchParams> at(const std::string& path) const {
        return at(path.c_str(), path.size());
    }

private:
    struct Node {
        std::string prefix;
        std::array<int16_t, 128> dispatch;  // first-byte → child index (-1 = none)
        std::vector<Node> children;
        int16_t param_child = -1;           // index of {param} child
        int16_t catch_all_child = -1;       // index of {param:path} child
        int route_index = -1;               // -1 = not a terminal node
        std::string param_name;             // non-empty if this is a {param} segment
        bool is_catch_all = false;          // true for {param:path}

        Node() { dispatch.fill(-1); }
    };

    // Phase A: O(1) for routes with no parameters
    std::unordered_map<std::string, int> static_routes_;

    // Phase B: Trie for parametric routes
    Node root_;

    // Internal recursive insert/match
    bool insert_recursive(Node& node, const std::string& path, int index, size_t pos);
    std::optional<MatchParams> match_recursive(
        const Node& node, const char* path, size_t len, size_t pos,
        std::vector<std::pair<std::string, std::string>>& params
    ) const;
};
