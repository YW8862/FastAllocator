#pragma once

#include <array>
#include <cstddef>

#include "config.h"

namespace my_alloc {

struct FreeList {
    void* head = nullptr;
    std::size_t size = 0;

    void Push(void* obj);
    void* Pop();
    bool Empty() const;
    void PushRange(void* start, void* end, std::size_t n);
    std::size_t PopRange(void*& start, void*& end, std::size_t batch);
};

class alignas(64) ThreadCache {
public:
    void* Allocate(std::size_t size);
    void Deallocate(void* ptr, std::size_t size);

    static ThreadCache& Current();

    // 仅用于当前阶段自测。
    FreeList& GetFreeListForTest(std::size_t index);

private:
    std::array<FreeList, kMaxSizeClass> free_lists_{};

    void FetchFromCentral(std::size_t index);
    void ReleaseToCentral(std::size_t index, FreeList& list);
};

}  // namespace my_alloc
