#pragma once

#include <cstddef>

namespace my_alloc {

struct Stats {
    std::size_t total_allocated = 0;
    std::size_t total_freed = 0;
    std::size_t current_bytes = 0;
    std::size_t peak_bytes = 0;
    std::size_t thread_count = 0;
    std::size_t active_thread_count = 0;
    std::size_t thread_total_allocated = 0;
    std::size_t thread_total_freed = 0;
    std::size_t thread_current_bytes = 0;
    std::size_t thread_peak_bytes = 0;
};

void Initialize();
void initialize();

void* Allocate(std::size_t size);
void Deallocate(void* ptr);
void* Reallocate(void* ptr, std::size_t size);

Stats GetStats();
Stats get_stats();

// 与设计文档中的接口命名保持一致。
void* malloc(std::size_t size);
void free(void* ptr);
void* realloc(void* ptr, std::size_t size);

}  // namespace my_alloc
