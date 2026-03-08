#pragma once

#include <cstddef>

namespace my_alloc {

// 基础页配置。
inline constexpr std::size_t kPageSize = 4096;

// 小对象分配由内存池的多级缓存体系处理。
inline constexpr std::size_t kMaxSmallObjectSize = 256 * 1024;

// Size Class 数量上限，供 ThreadCache/CentralCache 的数组容量使用。
inline constexpr std::size_t kMaxSizeClass = 128;

// 单个 Span 的页数上限，用于页级空闲链表管理。
inline constexpr std::size_t kMaxPages = 128;

// ThreadCache 与 CentralCache 之间的默认批量搬运数量。
inline constexpr std::size_t kBatchSize = 32;

// 每个线程在每个 size class 上可缓存的对象数量上限。
inline constexpr std::size_t kThreadCacheMaxCount = 256;

// 不同大小区间可使用的常见对齐粒度预设。
inline constexpr std::size_t kAlignmentSmall = 8;
inline constexpr std::size_t kAlignmentMedium = 16;
inline constexpr std::size_t kAlignmentLarge = 64;

// 调试模式：在用户块前后加 guard，并在分配/释放时填充固定字节。
inline constexpr bool kEnableDebugMode = true;
inline constexpr std::size_t kDebugGuardBytes = 16;
inline constexpr unsigned char kDebugAllocatedFillByte = 0xAA;
inline constexpr unsigned char kDebugFreedFillByte = 0xDD;

static_assert(kPageSize != 0, "kPageSize must be non-zero");
static_assert((kPageSize & (kPageSize - 1)) == 0,
              "kPageSize must be a power of two");
static_assert(kMaxSmallObjectSize >= kPageSize,
              "kMaxSmallObjectSize must be at least one page");
static_assert(kMaxSizeClass > 0, "kMaxSizeClass must be positive");
static_assert(kMaxPages > 0, "kMaxPages must be positive");
static_assert(kBatchSize > 0, "kBatchSize must be positive");
static_assert(kThreadCacheMaxCount >= kBatchSize,
              "kThreadCacheMaxCount should not be smaller than kBatchSize");
static_assert(kDebugGuardBytes >= alignof(std::max_align_t),
              "kDebugGuardBytes must preserve max alignment");
static_assert((kDebugGuardBytes % alignof(std::max_align_t)) == 0,
              "kDebugGuardBytes must be a multiple of max alignment");

}  // namespace my_alloc
