#pragma once
#include <vector>
#include <cstddef>

// Thread-local buffer pool — reuses heap allocations across requests.
// No RefCell needed in C++ — direct thread_local access.

constexpr size_t BUFFER_POOL_MAX = 4;        // 4 × 4 KB = 16 KB — single-threaded GIL server needs ≤ 2 in-flight
constexpr size_t BUFFER_INITIAL_CAPACITY = 4096;  // 4 KB: covers 99%+ of responses; grows on-demand

std::vector<char> acquire_buffer();
void release_buffer(std::vector<char> buf);
void prewarm_buffer_pool(int count);
