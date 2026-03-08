#include "../include/allocator/central_cache.h"

#include <cassert>

#include "../include/allocator/page_cache.h"
#include "../include/allocator/size_class.h"

namespace fastalloc {
namespace {

void*& NextObject(void* obj) {
    return *reinterpret_cast<void**>(obj);
}

}  // namespace

CentralCache& CentralCache::Instance() {
    static CentralCache instance;
    return instance;
}

std::size_t CentralCache::FetchRange(std::size_t index,
                                     std::size_t batch,
                                     void*& start,
                                     void*& end) {
    assert(index < kNumSizeClasses);

    start = nullptr;
    end = nullptr;
    if (batch == 0) {
        return 0;
    }

    CentralBucket& bucket = buckets_[index];
    std::lock_guard<std::mutex> lock(bucket.lock);

    std::size_t fetched = 0;
    while (fetched < batch) {
        Span* span = bucket.span_list;
        if (span == nullptr) {
            span = FetchNewSpan(index);
            if (span == nullptr) {
                break;
            }
        }

        std::size_t taken_from_span = 0;
        while (span->free_list != nullptr && fetched < batch) {
            void* obj = span->free_list;
            span->free_list = NextObject(obj);
            NextObject(obj) = nullptr;

            if (start == nullptr) {
                start = obj;
            } else {
                NextObject(end) = obj;
            }
            end = obj;
            ++fetched;
            ++taken_from_span;
            --span->free_count;
        }

        span->use_count.fetch_add(taken_from_span, std::memory_order_relaxed);

        if (span->free_list == nullptr) {
            RemoveSpan(bucket, span);
        }

        if (taken_from_span == 0) {
            break;
        }
    }

    return fetched;
}

void CentralCache::ReturnRange(std::size_t index,
                               void* start,
                               void* end,
                               std::size_t n) {
    assert(index < kNumSizeClasses);

    if (start == nullptr || n == 0) {
        return;
    }

    CentralBucket& bucket = buckets_[index];
    std::lock_guard<std::mutex> lock(bucket.lock);

    (void)end;
    void* current = start;
    for (std::size_t i = 0; i < n; ++i) {
        void* next = NextObject(current);

        Span* span = PageCache::Instance().ResolveSpan(current);
        assert(span != nullptr);

        const bool was_empty = (span->free_list == nullptr);
        NextObject(current) = span->free_list;
        span->free_list = current;
        ++span->free_count;

        const std::size_t old_use_count =
            span->use_count.fetch_sub(1, std::memory_order_relaxed);
        assert(old_use_count > 0);

        if (was_empty) {
            InsertSpan(bucket, span);
        }

        const std::size_t new_use_count = old_use_count - 1;
        if (new_use_count == 0 && span->free_count == span->objects_per_span) {
            RemoveSpan(bucket, span);
            ReleaseSpan(span);
        }

        current = next;
    }
}

Span* CentralCache::FetchNewSpan(std::size_t index) {
    Span* span = PageCache::Instance().NewSpan(index);
    if (span == nullptr) {
        return nullptr;
    }

    const std::size_t object_size = IndexToSize(index);
    char* begin = static_cast<char*>(PageIdToPtr(span->page_id));
    void* first = nullptr;
    void* last = nullptr;

    for (std::size_t i = 0; i < span->objects_per_span; ++i) {
        void* obj = begin + i * object_size;
        NextObject(obj) = nullptr;

        if (first == nullptr) {
            first = obj;
        } else {
            NextObject(last) = obj;
        }
        last = obj;
    }

    span->free_list = first;
    span->free_count = span->objects_per_span;
    PageCache::Instance().GetRadixTree().SetSpanRange(span);

    InsertSpan(buckets_[index], span);
    return span;
}

void CentralCache::ReleaseSpan(Span* span) {
    PageCache::Instance().ReleaseSpan(span);
}

void CentralCache::InsertSpan(CentralBucket& bucket, Span* span) {
    if (span == nullptr) {
        return;
    }

    if (span->prev != nullptr || bucket.span_list == span) {
        return;
    }

    span->next = bucket.span_list;
    span->prev = nullptr;
    if (bucket.span_list != nullptr) {
        bucket.span_list->prev = span;
    }
    bucket.span_list = span;
}

void CentralCache::RemoveSpan(CentralBucket& bucket, Span* span) {
    if (span == nullptr) {
        return;
    }

    if (span->prev != nullptr) {
        span->prev->next = span->next;
    } else if (bucket.span_list == span) {
        bucket.span_list = span->next;
    } else {
        return;
    }

    if (span->next != nullptr) {
        span->next->prev = span->prev;
    }

    span->next = nullptr;
    span->prev = nullptr;
}

}  // namespace fastalloc
