#include "buffer_pool.hpp"

static thread_local std::vector<std::vector<char>> pool;

std::vector<char> acquire_buffer() {
    if (!pool.empty()) {
        auto buf = std::move(pool.back());
        pool.pop_back();
        buf.clear();
        return buf;
    }
    std::vector<char> buf;
    buf.reserve(BUFFER_INITIAL_CAPACITY);
    return buf;
}

void release_buffer(std::vector<char> buf) {
    if (pool.size() < BUFFER_POOL_MAX) {
        // Shrink oversized buffers before returning to pool
        if (buf.capacity() > BUFFER_INITIAL_CAPACITY * 4) {
            buf.resize(0);
            buf.shrink_to_fit();
            buf.reserve(BUFFER_INITIAL_CAPACITY);
        }
        pool.push_back(std::move(buf));
    }
    // else: buf is dropped (freed) — pool is full
}
