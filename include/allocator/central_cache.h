#pragma once

#include <array>
#include <cstddef>
#include <mutex>

#include "config.h"
#include "span.h"

namespace my_alloc {

struct CentralBucket {
    std::mutex lock;
    Span* span_list = nullptr;
};

class CentralCache {
public:
    static CentralCache& Instance();

    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    // 从 Central 取出一段对象链表，返回实际获取数量。
    std::size_t FetchRange(std::size_t index,
                           std::size_t batch,
                           void*& start,
                           void*& end);

    // 将一段对象链表归还给 Central，并根据 Span 状态决定是否释放。
    void ReturnRange(std::size_t index, void* start, void* end, std::size_t n);

private:
    CentralCache() = default;

    Span* FetchNewSpan(std::size_t index);
    void ReleaseSpan(Span* span);
    void InsertSpan(CentralBucket& bucket, Span* span);
    void RemoveSpan(CentralBucket& bucket, Span* span);

    std::array<CentralBucket, kMaxSizeClass> buckets_{};
};

}  // namespace my_alloc
