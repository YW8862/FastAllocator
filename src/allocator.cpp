#include "../include/allocator/allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <vector>
#include <unordered_map>

#include "../include/allocator/central_cache.h"
#include "../include/allocator/config.h"
#include "../include/allocator/page_cache.h"
#include "../include/allocator/size_class.h"
#include "../include/allocator/span.h"
#include "../include/allocator/thread_cache.h"

namespace my_alloc {
namespace {

std::atomic<std::size_t> g_total_allocated{0};
std::atomic<std::size_t> g_total_freed{0};
std::atomic<std::size_t> g_current_bytes{0};
std::atomic<std::size_t> g_peak_bytes{0};
std::once_flag g_initialize_once;
std::mutex g_thread_stats_lock;
std::mutex g_debug_allocations_lock;

struct ThreadStatsEntry {
    std::atomic<std::size_t> total_allocated{0};
    std::atomic<std::size_t> total_freed{0};
    std::atomic<std::size_t> current_bytes{0};
    std::atomic<std::size_t> peak_bytes{0};
    std::atomic<bool> active{true};
};

struct AllocationRecord {
    void* raw_ptr = nullptr;
    std::size_t requested_size = 0;
    std::size_t accounted_bytes = 0;
    ThreadStatsEntry* owner_thread = nullptr;
    bool allocated = false;
};

std::vector<ThreadStatsEntry*> g_thread_stats_entries;
std::unordered_map<void*, AllocationRecord> g_allocations;

constexpr std::uint64_t kDebugGuardMagic = 0xD3B6A5C4E7F8091ULL;

void UpdatePeak(std::size_t current_bytes) {
    std::size_t peak = g_peak_bytes.load(std::memory_order_relaxed);
    while (current_bytes > peak &&
           !g_peak_bytes.compare_exchange_weak(
               peak, current_bytes, std::memory_order_relaxed)) {
    }
}

void UpdateThreadPeak(ThreadStatsEntry* entry, std::size_t current_bytes) {
    std::size_t peak = entry->peak_bytes.load(std::memory_order_relaxed);
    while (current_bytes > peak &&
           !entry->peak_bytes.compare_exchange_weak(
               peak, current_bytes, std::memory_order_relaxed)) {
    }
}

void RecordAllocate(std::size_t bytes, ThreadStatsEntry* owner_thread) {
    g_total_allocated.fetch_add(bytes, std::memory_order_relaxed);
    const std::size_t current =
        g_current_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    UpdatePeak(current);

    if (owner_thread != nullptr) {
        owner_thread->total_allocated.fetch_add(bytes, std::memory_order_relaxed);
        const std::size_t thread_current =
            owner_thread->current_bytes.fetch_add(bytes, std::memory_order_relaxed) +
            bytes;
        UpdateThreadPeak(owner_thread, thread_current);
    }
}

void RecordFree(std::size_t bytes, ThreadStatsEntry* owner_thread) {
    g_total_freed.fetch_add(bytes, std::memory_order_relaxed);
    g_current_bytes.fetch_sub(bytes, std::memory_order_relaxed);

    if (owner_thread != nullptr) {
        owner_thread->total_freed.fetch_add(bytes, std::memory_order_relaxed);
        owner_thread->current_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

ThreadStatsEntry* CurrentThreadStats() {
    thread_local ThreadStatsEntry* tls_stats = [] {
        auto* entry = new ThreadStatsEntry();
        std::lock_guard<std::mutex> lock(g_thread_stats_lock);
        g_thread_stats_entries.push_back(entry);
        return entry;
    }();

    thread_local struct ThreadStatsLifetime {
        ThreadStatsEntry* entry;
        ~ThreadStatsLifetime() {
            entry->active.store(false, std::memory_order_relaxed);
        }
    } tls_lifetime{tls_stats};

    (void)tls_lifetime;
    return tls_stats;
}

Stats AggregateThreadStats() {
    Stats stats;
    std::lock_guard<std::mutex> lock(g_thread_stats_lock);
    stats.thread_count = g_thread_stats_entries.size();
    for (ThreadStatsEntry* entry : g_thread_stats_entries) {
        if (entry == nullptr) {
            continue;
        }
        if (entry->active.load(std::memory_order_relaxed)) {
            ++stats.active_thread_count;
        }
        stats.thread_total_allocated +=
            entry->total_allocated.load(std::memory_order_relaxed);
        stats.thread_total_freed +=
            entry->total_freed.load(std::memory_order_relaxed);
        stats.thread_current_bytes +=
            entry->current_bytes.load(std::memory_order_relaxed);
        stats.thread_peak_bytes += entry->peak_bytes.load(std::memory_order_relaxed);
    }
    return stats;
}

std::size_t LargeObjectPages(std::size_t size) {
    if (size > std::numeric_limits<std::size_t>::max() - (kPageSize - 1)) {
        return 0;
    }
    return (size + kPageSize - 1) / kPageSize;
}

bool AddOverflows(std::size_t lhs, std::size_t rhs) {
    return lhs > std::numeric_limits<std::size_t>::max() - rhs;
}

[[noreturn]] void DebugAbort(const char* reason, const void* ptr) {
    std::fprintf(stderr,
                 "FastAllocator debug check failed: %s (ptr=%p)\n",
                 reason, ptr);
    std::fflush(stderr);
    std::abort();
}

std::size_t DebugOverhead() {
    return kEnableDebugMode ? (2 * kDebugGuardBytes) : 0;
}

std::size_t InternalRequestSize(std::size_t requested_size) {
    if (!kEnableDebugMode) {
        return requested_size;
    }
    if (AddOverflows(requested_size, DebugOverhead())) {
        return 0;
    }
    return requested_size + DebugOverhead();
}

void FillGuard(unsigned char* guard) {
    for (std::size_t offset = 0; offset < kDebugGuardBytes;
         offset += sizeof(kDebugGuardMagic)) {
        const std::size_t chunk =
            std::min(sizeof(kDebugGuardMagic), kDebugGuardBytes - offset);
        std::memcpy(guard + offset, &kDebugGuardMagic, chunk);
    }
}

bool GuardMatches(const unsigned char* guard) {
    for (std::size_t offset = 0; offset < kDebugGuardBytes;
         offset += sizeof(kDebugGuardMagic)) {
        const std::size_t chunk =
            std::min(sizeof(kDebugGuardMagic), kDebugGuardBytes - offset);
        std::uint64_t observed = 0;
        std::memcpy(&observed, guard + offset, chunk);
        if (std::memcmp(&observed, &kDebugGuardMagic, chunk) != 0) {
            return false;
        }
    }
    return true;
}

unsigned char* RawBytes(void* ptr) {
    return static_cast<unsigned char*>(ptr);
}

void* UserPtrFromRaw(void* raw_ptr) {
    if (!kEnableDebugMode) {
        return raw_ptr;
    }
    return RawBytes(raw_ptr) + kDebugGuardBytes;
}

void InitializeDebugPayload(void* raw_ptr, std::size_t requested_size) {
    if (!kEnableDebugMode) {
        return;
    }

    unsigned char* bytes = RawBytes(raw_ptr);
    FillGuard(bytes);
    std::memset(bytes + kDebugGuardBytes, kDebugAllocatedFillByte, requested_size);
    FillGuard(bytes + kDebugGuardBytes + requested_size);
}

void VerifyDebugPayload(const AllocationRecord& record, const void* user_ptr) {
    if (!kEnableDebugMode) {
        return;
    }

    const unsigned char* bytes = RawBytes(record.raw_ptr);
    if (!GuardMatches(bytes)) {
        DebugAbort("buffer underrun detected", user_ptr);
    }
    if (!GuardMatches(bytes + kDebugGuardBytes + record.requested_size)) {
        DebugAbort("buffer overrun detected", user_ptr);
    }
}

void PoisonFreedPayload(const AllocationRecord& record) {
    if (!kEnableDebugMode) {
        return;
    }

    std::memset(RawBytes(record.raw_ptr) + kDebugGuardBytes,
                kDebugFreedFillByte,
                record.requested_size);
}

void RegisterAllocation(void* user_ptr,
                        void* raw_ptr,
                        std::size_t requested_size,
                        std::size_t accounted_bytes,
                        ThreadStatsEntry* owner_thread) {
    std::lock_guard<std::mutex> lock(g_debug_allocations_lock);
    g_allocations[user_ptr] = AllocationRecord{
        raw_ptr,
        requested_size,
        accounted_bytes,
        owner_thread,
        true,
    };
}

AllocationRecord GetLiveAllocation(const void* user_ptr) {
    std::lock_guard<std::mutex> lock(g_debug_allocations_lock);
    const auto it = g_allocations.find(const_cast<void*>(user_ptr));
    if (it == g_allocations.end()) {
        DebugAbort("invalid pointer", user_ptr);
    }
    if (!it->second.allocated) {
        DebugAbort("double free detected", user_ptr);
    }
    return it->second;
}

AllocationRecord MarkAllocationFreed(const void* user_ptr) {
    std::lock_guard<std::mutex> lock(g_debug_allocations_lock);
    const auto it = g_allocations.find(const_cast<void*>(user_ptr));
    if (it == g_allocations.end()) {
        DebugAbort("invalid pointer", user_ptr);
    }
    if (!it->second.allocated) {
        DebugAbort("double free detected", user_ptr);
    }
    it->second.allocated = false;
    return it->second;
}

void* LargeAllocate(std::size_t size) {
    const std::size_t page_num = LargeObjectPages(size);
    if (page_num == 0) {
        return nullptr;
    }
    const std::size_t bytes = page_num * kPageSize;

    void* memory =
        mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1, 0);
    if (memory == MAP_FAILED) {
        return nullptr;
    }

    Span* span = new (std::nothrow) Span();
    if (span == nullptr) {
        const int result = munmap(memory, bytes);
        assert(result == 0);
        return nullptr;
    }
    span->page_id = PtrToPageId(memory);
    span->page_num = page_num;
    span->object_size = 0;
    span->free_list = nullptr;
    span->use_count.store(0, std::memory_order_relaxed);
    span->objects_per_span = 1;
    span->free_count = 0;
    span->is_free = false;
    span->next = nullptr;
    span->prev = nullptr;

    PageCache::Instance().GetRadixTree().SetSpanRange(span);
    return memory;
}

void LargeFree(Span* span) {
    assert(span != nullptr);
    PageCache::Instance().ReleaseSpan(span);
}

}  // namespace

void Initialize() {
    std::call_once(g_initialize_once, [] {
        (void)CentralCache::Instance();
        (void)PageCache::Instance();
    });
}

void initialize() {
    Initialize();
}

void* Allocate(std::size_t size) {
    Initialize();

    if (size == 0) {
        return nullptr;
    }

    const std::size_t internal_size = InternalRequestSize(size);
    if (internal_size == 0) {
        return nullptr;
    }

    const std::size_t accounted_bytes =
        (internal_size > kMaxSmallObjectSize)
            ? (LargeObjectPages(internal_size) * kPageSize)
            : IndexToSize(SizeClassIndex(internal_size));
    ThreadStatsEntry* owner_thread = CurrentThreadStats();

    void* raw_ptr = nullptr;
    if (internal_size > kMaxSmallObjectSize) {
        raw_ptr = LargeAllocate(internal_size);
    } else {
        raw_ptr = ThreadCache::Current().Allocate(internal_size);
    }

    if (raw_ptr == nullptr) {
        return nullptr;
    }

    RecordAllocate(accounted_bytes, owner_thread);

    if (!kEnableDebugMode) {
        RegisterAllocation(raw_ptr, raw_ptr, size, accounted_bytes, owner_thread);
        return raw_ptr;
    }

    InitializeDebugPayload(raw_ptr, size);
    void* user_ptr = UserPtrFromRaw(raw_ptr);
    RegisterAllocation(user_ptr, raw_ptr, size, accounted_bytes, owner_thread);
    return user_ptr;
}

void Deallocate(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    Initialize();

    AllocationRecord record{};
    if (kEnableDebugMode) {
        record = MarkAllocationFreed(ptr);
        VerifyDebugPayload(record, ptr);
        PoisonFreedPayload(record);
        ptr = record.raw_ptr;
    } else {
        record = MarkAllocationFreed(ptr);
    }

    Span* span = PageCache::Instance().ResolveSpan(ptr);
    assert(span != nullptr);

    if (span->object_size == 0) {
        LargeFree(span);
        RecordFree(record.accounted_bytes, record.owner_thread);
        return;
    }

    ThreadCache::Current().Deallocate(ptr, span->object_size);
    RecordFree(record.accounted_bytes, record.owner_thread);
}

void* Reallocate(void* ptr, std::size_t size) {
    Initialize();

    if (ptr == nullptr) {
        return Allocate(size);
    }

    if (size == 0) {
        Deallocate(ptr);
        return nullptr;
    }

    std::size_t old_size = 0;
    if (kEnableDebugMode) {
        const AllocationRecord record = GetLiveAllocation(ptr);
        VerifyDebugPayload(record, ptr);
        old_size = record.requested_size;
    } else {
        old_size = GetLiveAllocation(ptr).requested_size;
    }

    if (size <= old_size) {
        return ptr;
    }

    void* new_ptr = Allocate(size);
    if (new_ptr == nullptr) {
        return nullptr;
    }

    std::memcpy(new_ptr, ptr, std::min(old_size, size));
    Deallocate(ptr);
    return new_ptr;
}

Stats GetStats() {
    Stats stats;
    stats.total_allocated = g_total_allocated.load(std::memory_order_relaxed);
    stats.total_freed = g_total_freed.load(std::memory_order_relaxed);
    stats.current_bytes = g_current_bytes.load(std::memory_order_relaxed);
    stats.peak_bytes = g_peak_bytes.load(std::memory_order_relaxed);
    const Stats thread_stats = AggregateThreadStats();
    stats.thread_count = thread_stats.thread_count;
    stats.active_thread_count = thread_stats.active_thread_count;
    stats.thread_total_allocated = thread_stats.thread_total_allocated;
    stats.thread_total_freed = thread_stats.thread_total_freed;
    stats.thread_current_bytes = thread_stats.thread_current_bytes;
    stats.thread_peak_bytes = thread_stats.thread_peak_bytes;
    return stats;
}

Stats get_stats() {
    return GetStats();
}

void* malloc(std::size_t size) {
    return Allocate(size);
}

void free(void* ptr) {
    Deallocate(ptr);
}

void* realloc(void* ptr, std::size_t size) {
    return Reallocate(ptr, size);
}

}  // namespace my_alloc
