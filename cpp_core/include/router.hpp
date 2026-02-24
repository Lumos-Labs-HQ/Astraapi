#pragma once
#include <string>
#include <string_view>
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
//
// Zero-allocation matching: MatchParams uses string_view into request buffer.

struct MatchParams {
    int route_index = -1;
    struct Param {
        std::string_view name;   // points into trie node (stable after startup)
        std::string_view value;  // points into request buffer (valid for request lifetime)
    };
    static constexpr int MAX_INLINE = 8;
    Param params[MAX_INLINE];
    int param_count = 0;

    void add(std::string_view n, std::string_view v) {
        if (param_count < MAX_INLINE) params[param_count++] = {n, v};
    }
};

// Transparent hash/equal for string_view lookups on std::string keys
struct SVHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};
struct SVEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
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
        bool has_dispatch_collision = false; // true if multiple children share same first byte

        Node() { dispatch.fill(-1); }
    };

    // Phase A: O(1) for routes with no parameters — transparent string_view lookup
    std::unordered_map<std::string, int, SVHash, SVEqual> static_routes_;

    // Phase B: Trie for parametric routes
    Node root_;

    // Internal insert (recursive — only at startup)
    bool insert_recursive(Node& node, const std::string& path, int index, size_t pos);

    // Recursive match — zero allocation, with backtracking
    bool match_recursive(const Node& node, const char* path, size_t len,
                         size_t pos, MatchParams& result) const;
};
