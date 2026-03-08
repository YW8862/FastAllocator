#pragma once

#include <array>
#include <cstddef>

#include "config.h"

namespace my_alloc {

constexpr std::size_t AlignSize(std::size_t size,
                                std::size_t align = kAlignmentSmall) {
    if (size == 0) {
        return 0;
    }
    return (size + align - 1) & ~(align - 1);
}

namespace detail {

constexpr std::size_t kLookupGranularity = 8;
constexpr std::size_t kLookupTableSize =
    kMaxSmallObjectSize / kLookupGranularity;

constexpr std::size_t CountRange(std::size_t begin,
                                 std::size_t end,
                                 std::size_t step) {
    return ((end - begin) / step) + 1;
}

constexpr std::size_t kRange1Count = CountRange(8, 128, 8);
constexpr std::size_t kRange2Count = CountRange(160, 1024, 32);
constexpr std::size_t kRange3Count = CountRange(1280, 4096, 256);
constexpr std::size_t kRange4Count = CountRange(5120, 16384, 1024);
constexpr std::size_t kRange5Count = CountRange(20480, 65536, 4096);
constexpr std::size_t kRange6Count = CountRange(81920, 262144, 16384);

constexpr std::size_t kNumSizeClasses = kRange1Count + kRange2Count +
                                        kRange3Count + kRange4Count +
                                        kRange5Count + kRange6Count;

constexpr std::array<std::size_t, kNumSizeClasses> BuildSizeClasses() {
    std::array<std::size_t, kNumSizeClasses> table{};
    std::size_t index = 0;

    for (std::size_t size = 8; size <= 128; size += 8) {
        table[index++] = size;
    }
    for (std::size_t size = 160; size <= 1024; size += 32) {
        table[index++] = size;
    }
    for (std::size_t size = 1280; size <= 4096; size += 256) {
        table[index++] = size;
    }
    for (std::size_t size = 5120; size <= 16384; size += 1024) {
        table[index++] = size;
    }
    for (std::size_t size = 20480; size <= 65536; size += 4096) {
        table[index++] = size;
    }
    for (std::size_t size = 81920; size <= 262144; size += 16384) {
        table[index++] = size;
    }

    return table;
}

inline constexpr auto kSizeClasses = BuildSizeClasses();

constexpr std::array<std::size_t, kLookupTableSize + 1> BuildIndexLookup() {
    std::array<std::size_t, kLookupTableSize + 1> table{};
    std::size_t class_index = 0;

    for (std::size_t unit = 1; unit <= kLookupTableSize; ++unit) {
        const std::size_t request_size = unit * kLookupGranularity;
        while (class_index + 1 < kNumSizeClasses &&
               kSizeClasses[class_index] < request_size) {
            ++class_index;
        }
        table[unit] = class_index;
    }

    return table;
}

inline constexpr auto kIndexLookup = BuildIndexLookup();

}  // namespace detail

inline constexpr std::size_t kNumSizeClasses = detail::kNumSizeClasses;
inline constexpr auto kSizeClasses = detail::kSizeClasses;

static_assert(kNumSizeClasses <= kMaxSizeClass,
              "kNumSizeClasses exceeds kMaxSizeClass");

std::size_t SizeClassIndex(std::size_t size);
std::size_t IndexToSize(std::size_t index);
std::size_t NumToMove(std::size_t index);

}  // namespace my_alloc
