#include "../include/allocator/span.h"
#include "../include/allocator/allocator.h"
#include "../include/allocator/config.h"
#include "../include/allocator/central_cache.h"
#include "../include/allocator/page_cache.h"
#include "../include/allocator/radix_tree.h"
#include "../include/allocator/size_class.h"
#include "../include/allocator/thread_cache.h"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <thread>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

using my_alloc::Allocate;
using my_alloc::PageIdToPtr;
using my_alloc::PageCache;
using my_alloc::PtrToPageId;
using my_alloc::RadixTree;
using my_alloc::Span;
using my_alloc::SpanBytes;
using my_alloc::SpanContains;
using my_alloc::Stats;
using my_alloc::ThreadCache;
using my_alloc::SizeClassIndex;
using my_alloc::CentralCache;
using my_alloc::Deallocate;
using my_alloc::kPageSize;

std::size_t DebugAdjustedSize(std::size_t size) {
    if (!my_alloc::kEnableDebugMode) {
        return size;
    }
    return size + 2 * my_alloc::kDebugGuardBytes;
}

std::size_t AccountedSmallObjectBytes(std::size_t requested_size) {
    return my_alloc::IndexToSize(SizeClassIndex(DebugAdjustedSize(requested_size)));
}

void ExpectDebugAbort(void (*test_fn)()) {
    if (!my_alloc::kEnableDebugMode) {
        return;
    }

    const pid_t child = fork();
    assert(child >= 0);

    if (child == 0) {
        test_fn();
        _exit(0);
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    assert(waited == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void TriggerBufferOverrun() {
    char* ptr = static_cast<char*>(Allocate(32));
    assert(ptr != nullptr);
    ptr[32] = 'X';
    Deallocate(ptr);
}

void TriggerDoubleFree() {
    void* ptr = Allocate(48);
    assert(ptr != nullptr);
    Deallocate(ptr);
    Deallocate(ptr);
}

void TestPageIdRoundTrip() {
    void* memory =
        mmap(nullptr, 2 * kPageSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);

    const auto page_id = PtrToPageId(memory);
    assert(PageIdToPtr(page_id) == memory);

    const auto second_page =
        static_cast<void*>(static_cast<char*>(memory) + kPageSize);
    assert(PageIdToPtr(PtrToPageId(second_page)) == second_page);

    const int result = munmap(memory, 2 * kPageSize);
    assert(result == 0);
}

void TestSpanHelpers() {
    void* memory =
        mmap(nullptr, 3 * kPageSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);

    Span span;
    span.page_id = PtrToPageId(memory);
    span.page_num = 3;
    span.object_size = 64;

    assert(SpanBytes(span) == 3 * kPageSize);
    assert(SpanContains(span, memory));

    void* in_range = static_cast<void*>(static_cast<char*>(memory) + kPageSize);
    void* out_of_range =
        static_cast<void*>(static_cast<char*>(memory) + 3 * kPageSize);

    assert(SpanContains(span, in_range));
    assert(!SpanContains(span, out_of_range));

    const int result = munmap(memory, 3 * kPageSize);
    assert(result == 0);
}

void TestRadixTreeBasicLookup() {
    void* memory =
        mmap(nullptr, 3 * kPageSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);

    RadixTree tree;
    Span first_span;
    first_span.page_id = PtrToPageId(memory);
    first_span.page_num = 2;
    first_span.object_size = 64;

    Span second_span;
    second_span.page_id = PtrToPageId(static_cast<char*>(memory) + 2 * kPageSize);
    second_span.page_num = 1;
    second_span.object_size = 128;

    tree.SetSpanRange(&first_span);
    tree.SetSpan(PageIdToPtr(second_span.page_id), &second_span);

    assert(tree.GetSpan(memory) == &first_span);
    assert(tree.GetSpan(static_cast<char*>(memory) + 128) == &first_span);
    assert(tree.GetSpan(static_cast<char*>(memory) + kPageSize) == &first_span);
    assert(tree.GetSpan(static_cast<char*>(memory) + 2 * kPageSize) ==
           &second_span);

    tree.ClearSpan(static_cast<char*>(memory) + 2 * kPageSize);
    assert(tree.GetSpan(static_cast<char*>(memory) + 2 * kPageSize) == nullptr);
    assert(tree.GetSpan(memory) == &first_span);

    tree.ClearSpanRange(first_span);
    assert(tree.GetSpan(memory) == nullptr);
    assert(tree.GetSpan(static_cast<char*>(memory) + kPageSize) == nullptr);

    const int result = munmap(memory, 3 * kPageSize);
    assert(result == 0);
}

void TestFreeListPushPopRange() {
    my_alloc::FreeList list;

    std::uintptr_t a = 0;
    std::uintptr_t b = 0;
    std::uintptr_t c = 0;

    list.Push(&a);
    list.Push(&b);
    list.Push(&c);

    assert(list.size == 3);
    assert(list.Pop() == &c);
    assert(list.size == 2);

    void* start = nullptr;
    void* end = nullptr;
    const std::size_t popped = list.PopRange(start, end, 2);
    assert(popped == 2);
    assert(start == &b);
    assert(end == &a);
    assert(list.Empty());

    list.PushRange(start, end, popped);
    assert(list.size == 2);
    assert(list.Pop() == &b);
    assert(list.Pop() == &a);
    assert(list.Empty());
}

void TestThreadCacheReuse() {
    ThreadCache cache;
    void* first = cache.Allocate(24);
    void* second = cache.Allocate(24);

    assert(first != nullptr);
    assert(second != nullptr);
    assert(first != second);

    cache.Deallocate(first, 24);
    cache.Deallocate(second, 24);

    void* reused_a = cache.Allocate(24);
    void* reused_b = cache.Allocate(24);

    assert(reused_a == second);
    assert(reused_b == first);

    cache.Deallocate(reused_a, 24);
    cache.Deallocate(reused_b, 24);
}

void TestThreadCacheReleaseThreshold() {
    ThreadCache cache;
    const std::size_t index = SizeClassIndex(8);
    const std::size_t batch = my_alloc::NumToMove(index);

    for (std::size_t i = 0; i < batch * 2; ++i) {
        void* ptr = cache.Allocate(8);
        assert(ptr != nullptr);
        cache.Deallocate(ptr, 8);
    }

    assert(cache.GetFreeListForTest(index).size < batch * 2);
}

void TestCentralCacheFetchAndReturn() {
    CentralCache& central = CentralCache::Instance();
    const std::size_t index = SizeClassIndex(64);

    void* start = nullptr;
    void* end = nullptr;
    const std::size_t fetched = central.FetchRange(index, 16, start, end);
    assert(fetched == 16);
    assert(start != nullptr);
    assert(end != nullptr);

    Span* span = PageCache::Instance().ResolveSpan(start);
    assert(span != nullptr);
    assert(span->object_size == my_alloc::IndexToSize(index));

    void* current = start;
    for (std::size_t i = 0; i < fetched; ++i) {
        assert(PageCache::Instance().ResolveSpan(current) == span);
        current = (i + 1 < fetched) ? *reinterpret_cast<void**>(current) : nullptr;
    }

    central.ReturnRange(index, start, end, fetched);

    void* second_start = nullptr;
    void* second_end = nullptr;
    const std::size_t fetched_again =
        central.FetchRange(index, 8, second_start, second_end);
    assert(fetched_again == 8);
    assert(second_start != nullptr);
    assert(PageCache::Instance().ResolveSpan(second_start) != nullptr);

    central.ReturnRange(index, second_start, second_end, fetched_again);
}

void TestCentralCacheReleaseFullSpan() {
    CentralCache& central = CentralCache::Instance();
    const std::size_t index = SizeClassIndex(128);

    void* start = nullptr;
    void* end = nullptr;
    const std::size_t fetched = central.FetchRange(index, 32, start, end);
    assert(fetched == 32);

    Span* span = PageCache::Instance().ResolveSpan(start);
    assert(span != nullptr);
    const auto released_page_id = span->page_id;

    void* full_start = start;
    void* full_end = end;
    std::size_t full_count = fetched;

    while (full_count < span->objects_per_span) {
        void* extra_start = nullptr;
        void* extra_end = nullptr;
        const std::size_t extra =
            central.FetchRange(index, span->objects_per_span - full_count,
                               extra_start, extra_end);
        assert(extra > 0);

        *reinterpret_cast<void**>(full_end) = extra_start;
        full_end = extra_end;
        full_count += extra;
    }

    central.ReturnRange(index, full_start, full_end, full_count);
    assert(PageCache::Instance().ResolveSpan(PageIdToPtr(released_page_id)) ==
           nullptr);
}

void TestAllocatorSmallPath() {
    my_alloc::initialize();

    Stats before = my_alloc::GetStats();
    unsigned char* p1 = static_cast<unsigned char*>(Allocate(24));
    void* p2 = Allocate(1000);

    assert(p1 != nullptr);
    assert(p2 != nullptr);
    assert(PageCache::Instance().ResolveSpan(p1) != nullptr);
    assert(PageCache::Instance().ResolveSpan(p2) != nullptr);
    assert(PageCache::Instance().ResolveSpan(p1)->object_size >= DebugAdjustedSize(24));
    assert(PageCache::Instance().ResolveSpan(p2)->object_size >= DebugAdjustedSize(1000));

    if (my_alloc::kEnableDebugMode) {
        for (std::size_t i = 0; i < 24; ++i) {
            assert(p1[i] == my_alloc::kDebugAllocatedFillByte);
        }
    }

    Deallocate(p1);
    Deallocate(p2);

    Stats after = my_alloc::GetStats();
    assert(after.total_allocated > before.total_allocated);
    assert(after.total_freed > before.total_freed);
}

void TestAllocatorLargePath() {
    constexpr std::size_t kLargeBytes = 300 * 1024;

    Stats before = my_alloc::get_stats();
    void* large_a = my_alloc::malloc(kLargeBytes);
    void* large_b = my_alloc::malloc(kLargeBytes + 123);
    void* small = my_alloc::malloc(64);
    assert(large_a != nullptr);
    assert(large_b != nullptr);
    assert(small != nullptr);

    Span* large_span_a = PageCache::Instance().ResolveSpan(large_a);
    Span* large_span_b = PageCache::Instance().ResolveSpan(large_b);
    Span* small_span = PageCache::Instance().ResolveSpan(small);
    assert(large_span_a != nullptr);
    assert(large_span_b != nullptr);
    assert(small_span != nullptr);
    assert(large_span_a->object_size == 0);
    assert(large_span_b->object_size == 0);
    assert(small_span->object_size != 0);
    assert(large_span_a != large_span_b);

    const std::size_t expected_pages_a =
        (DebugAdjustedSize(kLargeBytes) + kPageSize - 1) / kPageSize;
    assert(large_span_a->page_num == expected_pages_a);
    assert(SpanContains(*large_span_a, large_a));
    assert(SpanContains(*large_span_b, large_b));
    assert(!SpanContains(*large_span_a, small));
    assert(!SpanContains(*small_span, large_a));
    assert(!SpanContains(*large_span_a, large_b));
    assert(!SpanContains(*large_span_b, large_a));

    const auto page_id_a = large_span_a->page_id;
    const auto page_id_b = large_span_b->page_id;

    my_alloc::free(large_a);
    my_alloc::free(large_b);
    my_alloc::free(small);

    Stats after = my_alloc::get_stats();
    assert(after.total_allocated >=
           before.total_allocated + expected_pages_a * kPageSize);
    assert(after.total_freed >= before.total_freed + expected_pages_a * kPageSize);
    assert(PageCache::Instance().ResolveSpan(PageIdToPtr(page_id_a)) == nullptr);
    assert(PageCache::Instance().ResolveSpan(PageIdToPtr(page_id_b)) == nullptr);
}

void TestAllocatorReallocate() {
    char* ptr = static_cast<char*>(Allocate(32));
    assert(ptr != nullptr);

    const char payload[] = "fast-allocator";
    std::memcpy(ptr, payload, sizeof(payload));

    char* new_ptr = static_cast<char*>(my_alloc::Reallocate(ptr, 2048));
    assert(new_ptr != nullptr);
    assert(std::memcmp(new_ptr, payload, sizeof(payload)) == 0);

    Deallocate(new_ptr);
}

void TestAllocatorApiAndLazyInitialize() {
    void* ptr = my_alloc::malloc(48);
    assert(ptr != nullptr);

    Span* span = PageCache::Instance().ResolveSpan(ptr);
    assert(span != nullptr);
    assert(span->object_size != 0);

    char* grown = static_cast<char*>(my_alloc::realloc(ptr, 96));
    assert(grown != nullptr);

    my_alloc::free(grown);

    Stats stats_lower = my_alloc::get_stats();
    Stats stats_upper = my_alloc::GetStats();
    assert(stats_lower.total_allocated == stats_upper.total_allocated);
    assert(stats_lower.total_freed == stats_upper.total_freed);
    assert(stats_lower.current_bytes == stats_upper.current_bytes);
    assert(stats_lower.peak_bytes == stats_upper.peak_bytes);
}

void TestAllocatorBoundarySizes() {
    assert(my_alloc::malloc(0) == nullptr);

    void* one_byte = my_alloc::malloc(1);
    assert(one_byte != nullptr);
    Span* one_byte_span = PageCache::Instance().ResolveSpan(one_byte);
    assert(one_byte_span != nullptr);
    assert(one_byte_span->object_size >= DebugAdjustedSize(1));
    my_alloc::free(one_byte);

    void* max_small = my_alloc::malloc(my_alloc::kMaxSmallObjectSize);
    assert(max_small != nullptr);
    Span* max_small_span = PageCache::Instance().ResolveSpan(max_small);
    assert(max_small_span != nullptr);
    if (DebugAdjustedSize(my_alloc::kMaxSmallObjectSize) >
        my_alloc::kMaxSmallObjectSize) {
        assert(max_small_span->object_size == 0);
    } else {
        assert(max_small_span->object_size != 0);
        assert(max_small_span->object_size >=
               DebugAdjustedSize(my_alloc::kMaxSmallObjectSize));
    }
    my_alloc::free(max_small);

    void* large = my_alloc::malloc(my_alloc::kMaxSmallObjectSize + 1);
    assert(large != nullptr);
    Span* large_span = PageCache::Instance().ResolveSpan(large);
    assert(large_span != nullptr);
    assert(large_span->object_size == 0);
    my_alloc::free(large);
}

void TestAllocatorDebugChecks() {
    ExpectDebugAbort(&TriggerBufferOverrun);
    ExpectDebugAbort(&TriggerDoubleFree);
}

void TestAllocatorStatsAccounting() {
    constexpr std::size_t kRequestSize = 24;
    constexpr std::size_t kAllocCount = 10;
    const std::size_t accounted_bytes = AccountedSmallObjectBytes(kRequestSize);

    Stats before = my_alloc::GetStats();
    std::vector<void*> ptrs;
    ptrs.reserve(kAllocCount);

    for (std::size_t i = 0; i < kAllocCount; ++i) {
        void* ptr = my_alloc::malloc(kRequestSize);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
    }

    Stats mid = my_alloc::GetStats();
    assert(mid.total_allocated - before.total_allocated ==
           kAllocCount * accounted_bytes);
    assert(mid.current_bytes - before.current_bytes ==
           kAllocCount * accounted_bytes);
    assert(mid.thread_total_allocated - before.thread_total_allocated ==
           kAllocCount * accounted_bytes);
    assert(mid.thread_current_bytes - before.thread_current_bytes ==
           kAllocCount * accounted_bytes);
    assert(mid.thread_total_allocated == mid.total_allocated);
    assert(mid.thread_total_freed == mid.total_freed);
    assert(mid.thread_current_bytes == mid.current_bytes);
    assert(mid.thread_peak_bytes >= mid.thread_current_bytes);
    assert(mid.thread_count >= 1);
    assert(mid.active_thread_count >= 1);

    for (std::size_t i = 0; i < kAllocCount / 2; ++i) {
        my_alloc::free(ptrs[i]);
    }

    Stats after = my_alloc::GetStats();
    assert(after.total_freed - before.total_freed ==
           (kAllocCount / 2) * accounted_bytes);
    assert(after.current_bytes - before.current_bytes ==
           (kAllocCount / 2) * accounted_bytes);
    assert(after.thread_total_freed - before.thread_total_freed ==
           (kAllocCount / 2) * accounted_bytes);
    assert(after.thread_current_bytes - before.thread_current_bytes ==
           (kAllocCount / 2) * accounted_bytes);
    assert(after.thread_total_allocated == after.total_allocated);
    assert(after.thread_total_freed == after.total_freed);
    assert(after.thread_current_bytes == after.current_bytes);

    for (std::size_t i = kAllocCount / 2; i < kAllocCount; ++i) {
        my_alloc::free(ptrs[i]);
    }
}

void StatsWorker(std::size_t request_size, std::size_t count) {
    std::vector<void*> ptrs;
    ptrs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        void* ptr = my_alloc::malloc(request_size);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
    }
    for (void* ptr : ptrs) {
        my_alloc::free(ptr);
    }
}

void TestAllocatorThreadStatsAggregation() {
    constexpr std::size_t kRequestSize = 64;
    constexpr std::size_t kAllocCount = 8;
    const std::size_t accounted_bytes = AccountedSmallObjectBytes(kRequestSize);

    Stats before = my_alloc::GetStats();
    std::thread worker(StatsWorker, kRequestSize, kAllocCount);
    worker.join();

    Stats after = my_alloc::GetStats();
    assert(after.total_allocated - before.total_allocated ==
           kAllocCount * accounted_bytes);
    assert(after.total_freed - before.total_freed ==
           kAllocCount * accounted_bytes);
    assert(after.current_bytes == before.current_bytes);
    assert(after.thread_total_allocated == after.total_allocated);
    assert(after.thread_total_freed == after.total_freed);
    assert(after.thread_current_bytes == after.current_bytes);
    assert(after.thread_count >= before.thread_count + 1);
    assert(after.active_thread_count >= 1);
    assert(after.active_thread_count <= after.thread_count);
}

void SameSizeWorker(std::size_t iterations, std::size_t request_size) {
    std::vector<void*> ptrs;
    ptrs.reserve(32);
    for (std::size_t i = 0; i < iterations; ++i) {
        void* ptr = my_alloc::malloc(request_size);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
        if (ptrs.size() == 32) {
            for (void* p : ptrs) {
                my_alloc::free(p);
            }
            ptrs.clear();
        }
    }
    for (void* p : ptrs) {
        my_alloc::free(p);
    }
}

void MixedSizeWorker(std::size_t iterations, std::size_t seed) {
    const std::size_t sizes[] = {
        1,
        8,
        24,
        64,
        256,
        1024,
        8 * 1024,
        64 * 1024,
        my_alloc::kMaxSmallObjectSize,
        my_alloc::kMaxSmallObjectSize + 1,
    };

    std::vector<void*> ptrs;
    ptrs.reserve(16);
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t request_size =
            sizes[(i + seed) % (sizeof(sizes) / sizeof(sizes[0]))];
        void* ptr = my_alloc::malloc(request_size);
        assert(ptr != nullptr);
        ptrs.push_back(ptr);
        if (ptrs.size() == 16) {
            my_alloc::free(ptrs.front());
            ptrs.erase(ptrs.begin());
        }
    }
    for (void* p : ptrs) {
        my_alloc::free(p);
    }
}

void TestAllocatorMultithreaded() {
    Stats before_same = my_alloc::GetStats();
    std::vector<std::thread> same_size_threads;
    for (std::size_t i = 0; i < 4; ++i) {
        same_size_threads.emplace_back(SameSizeWorker, 2000, 64);
    }
    for (auto& thread : same_size_threads) {
        thread.join();
    }
    Stats after_same = my_alloc::GetStats();
    assert(after_same.current_bytes == before_same.current_bytes);
    assert(after_same.thread_current_bytes == before_same.thread_current_bytes);

    Stats before_mixed = my_alloc::GetStats();
    std::vector<std::thread> mixed_threads;
    for (std::size_t i = 0; i < 4; ++i) {
        mixed_threads.emplace_back(MixedSizeWorker, 1500, i * 7);
    }
    for (auto& thread : mixed_threads) {
        thread.join();
    }
    Stats after_mixed = my_alloc::GetStats();
    assert(after_mixed.current_bytes == before_mixed.current_bytes);
    assert(after_mixed.thread_current_bytes == before_mixed.thread_current_bytes);
}

}  // namespace

int main() {
    TestPageIdRoundTrip();
    TestSpanHelpers();
    TestRadixTreeBasicLookup();
    TestFreeListPushPopRange();
    TestThreadCacheReuse();
    TestThreadCacheReleaseThreshold();
    TestCentralCacheFetchAndReturn();
    TestCentralCacheReleaseFullSpan();
    TestAllocatorSmallPath();
    TestAllocatorLargePath();
    TestAllocatorReallocate();
    TestAllocatorApiAndLazyInitialize();
    TestAllocatorBoundarySizes();
    TestAllocatorDebugChecks();
    TestAllocatorStatsAccounting();
    TestAllocatorThreadStatsAggregation();
    TestAllocatorMultithreaded();
    return 0;
}
