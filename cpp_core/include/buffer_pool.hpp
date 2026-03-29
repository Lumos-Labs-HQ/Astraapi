#pragma once
#include <vector>
#include <cstddef>

// Thread-local buffer pool — reuses heap allocations across requests.
// No RefCell needed in C++ — direct thread_local access.

constexpr size_t BUFFER_POOL_MAX = 32;       // 32 × 8 KB = 256 KB/thread — avoids burst malloc overflow
constexpr size_t BUFFER_INITIAL_CAPACITY = 8192;  // 8 KB: fewer grow() reallocations for medium responses

std::vector<char> acquire_buffer();
void release_buffer(std::vector<char> buf);
void prewarm_buffer_pool(int count);
void set_buffer_pool_max(size_t n);
