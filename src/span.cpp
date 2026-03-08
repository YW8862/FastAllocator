#include "../include/allocator/span.h"

#include <cassert>
#include <cstdint>

#include "../include/allocator/config.h"

namespace my_alloc {

void* PageIdToPtr(PageId page_id) {
    const std::uintptr_t address =
        static_cast<std::uintptr_t>(page_id) * kPageSize;
    return reinterpret_cast<void*>(address);
}

PageId PtrToPageId(const void* ptr) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    return static_cast<PageId>(address / kPageSize);
}

std::size_t SpanBytes(const Span& span) {
    return span.page_num * kPageSize;
}

bool SpanContains(const Span& span, const void* ptr) {
    if (span.page_num == 0 || ptr == nullptr) {
        return false;
    }

    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(PageIdToPtr(span.page_id));
    const std::uintptr_t end = begin + SpanBytes(span);
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    return address >= begin && address < end;
}

}  // namespace my_alloc
