#pragma once

#include <cstddef>
#include <mutex>

#include "radix_tree.h"
#include "span.h"

namespace my_alloc {

class PageCache {
public:
    static PageCache& Instance();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    // 为某个 size class 分配一个新的 Span。
    Span* NewSpan(std::size_t index);

    // 释放一个已经完全空闲的 Span。
    void ReleaseSpan(Span* span);

    RadixTree& GetRadixTree();
    Span* ResolveSpan(const void* ptr);

private:
    PageCache() = default;

    std::mutex lock_;
    RadixTree radix_tree_;
};

}  // namespace my_alloc
