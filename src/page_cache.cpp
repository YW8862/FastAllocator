#include "../include/allocator/page_cache.h"

#include <algorithm>
#include <cassert>
#include <new>
#include <sys/mman.h>

#include "../include/allocator/config.h"
#include "../include/allocator/size_class.h"

namespace my_alloc {
namespace {

std::size_t PagesForSizeClass(std::size_t index) {
    const std::size_t object_size = IndexToSize(index);
    const std::size_t target_objects = std::max<std::size_t>(NumToMove(index) * 8, 64);
    const std::size_t bytes = object_size * target_objects;
    const std::size_t pages = (bytes + kPageSize - 1) / kPageSize;
    return std::max<std::size_t>(1, pages);
}

}  // namespace

PageCache& PageCache::Instance() {
    static PageCache instance;
    return instance;
}

Span* PageCache::NewSpan(std::size_t index) {
    assert(index < kNumSizeClasses);

    const std::size_t page_num = PagesForSizeClass(index);
    const std::size_t bytes = page_num * kPageSize;

    std::lock_guard<std::mutex> lock(lock_);

    void* memory =
        mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1, 0);
    if (memory == MAP_FAILED) {
        return nullptr;
    }

    Span* span = new Span();
    span->page_id = PtrToPageId(memory);
    span->page_num = page_num;
    span->object_size = IndexToSize(index);
    span->free_list = nullptr;
    span->use_count.store(0, std::memory_order_relaxed);
    span->objects_per_span = bytes / span->object_size;
    span->free_count = 0;
    span->next = nullptr;
    span->prev = nullptr;

    return span;
}

void PageCache::ReleaseSpan(Span* span) {
    if (span == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(lock_);

    radix_tree_.ClearSpanRange(*span);

    void* memory = PageIdToPtr(span->page_id);
    const std::size_t bytes = span->page_num * kPageSize;
    const int result = munmap(memory, bytes);
    assert(result == 0);

    delete span;
}

RadixTree& PageCache::GetRadixTree() {
    return radix_tree_;
}

Span* PageCache::ResolveSpan(const void* ptr) {
    return radix_tree_.GetSpan(ptr);
}

}  // namespace my_alloc
