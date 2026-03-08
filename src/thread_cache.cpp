#include "../include/allocator/thread_cache.h"

#include <algorithm>
#include <cassert>

#include "../include/allocator/central_cache.h"
#include "../include/allocator/size_class.h"

namespace my_alloc {
namespace {

void*& NextObject(void* obj) {
    return *reinterpret_cast<void**>(obj);
}

}  // namespace

void FreeList::Push(void* obj) {
    assert(obj != nullptr);
    NextObject(obj) = head;
    head = obj;
    ++size;
}

void* FreeList::Pop() {
    if (head == nullptr) {
        return nullptr;
    }

    void* obj = head;
    head = NextObject(obj);
    NextObject(obj) = nullptr;
    --size;
    return obj;
}

bool FreeList::Empty() const {
    return head == nullptr;
}

void FreeList::PushRange(void* start, void* end, std::size_t n) {
    if (start == nullptr || end == nullptr || n == 0) {
        return;
    }

    NextObject(end) = head;
    head = start;
    size += n;
}

std::size_t FreeList::PopRange(void*& start, void*& end, std::size_t batch) {
    start = nullptr;
    end = nullptr;
    if (head == nullptr || batch == 0) {
        return 0;
    }

    const std::size_t count = std::min(batch, size);
    start = head;

    void* current = head;
    for (std::size_t i = 1; i < count; ++i) {
        current = NextObject(current);
    }

    end = current;
    head = NextObject(current);
    NextObject(current) = nullptr;
    size -= count;
    return count;
}

void* ThreadCache::Allocate(std::size_t size) {
    assert(size > 0);
    assert(size <= kMaxSmallObjectSize);

    const std::size_t index = SizeClassIndex(size);
    FreeList& list = free_lists_[index];
    if (list.Empty()) {
        FetchFromCentral(index);
    }

    return list.Pop();
}

void ThreadCache::Deallocate(void* ptr, std::size_t size) {
    if (ptr == nullptr) {
        return;
    }

    assert(size > 0);
    assert(size <= kMaxSmallObjectSize);

    const std::size_t index = SizeClassIndex(size);
    FreeList& list = free_lists_[index];
    list.Push(ptr);
    ReleaseToCentral(index, list);
}

ThreadCache& ThreadCache::Current() {
    thread_local ThreadCache tls_thread_cache;
    return tls_thread_cache;
}

FreeList& ThreadCache::GetFreeListForTest(std::size_t index) {
    assert(index < kNumSizeClasses);
    return free_lists_[index];
}

void ThreadCache::FetchFromCentral(std::size_t index) {
    assert(index < kNumSizeClasses);

    void* start = nullptr;
    void* end = nullptr;
    const std::size_t batch = NumToMove(index);
    const std::size_t fetched =
        CentralCache::Instance().FetchRange(index, batch, start, end);
    free_lists_[index].PushRange(start, end, fetched);
}

void ThreadCache::ReleaseToCentral(std::size_t index, FreeList& list) {
    assert(index < kNumSizeClasses);

    const std::size_t threshold = NumToMove(index) * 2;
    if (list.size < threshold) {
        return;
    }

    void* start = nullptr;
    void* end = nullptr;
    const std::size_t returned = list.PopRange(start, end, NumToMove(index));
    CentralCache::Instance().ReturnRange(index, start, end, returned);
}

}  // namespace my_alloc
