#pragma once

#include <atomic>
#include <cstddef>

namespace fastalloc {

using PageId = std::size_t;

// Span 表示一段连续页，以及这段页被切分成小对象后的元数据。
struct Span {
    PageId page_id = 0;
    std::size_t page_num = 0;
    std::size_t object_size = 0;
    void* free_list = nullptr;
    std::atomic<std::size_t> use_count{0};
    std::size_t objects_per_span = 0;
    std::size_t free_count = 0;
    bool is_free = false;
    Span* next = nullptr;
    Span* prev = nullptr;
};

// 将页号转换为页起始地址。当前版本直接基于虚拟地址计算，不依赖额外基址。
void* PageIdToPtr(PageId page_id);

// 将任意地址转换为所属页号，结果为该地址所在页的页编号。
PageId PtrToPageId(const void* ptr);

// 返回 Span 覆盖的总字节数。
std::size_t SpanBytes(const Span& span);

// 判断某个地址是否落在该 Span 覆盖的地址范围内。
bool SpanContains(const Span& span, const void* ptr);

}  // namespace fastalloc
