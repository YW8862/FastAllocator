#include "../include/allocator/size_class.h"

#include <algorithm>
#include <cassert>

namespace my_alloc {

std::size_t SizeClassIndex(std::size_t size) {
    if (size == 0) {
        return 0;
    }

    if (size > kMaxSmallObjectSize) {
        return kNumSizeClasses - 1;
    }

    const std::size_t aligned_size =
        AlignSize(size, detail::kLookupGranularity);
    const std::size_t lookup_index =
        aligned_size / detail::kLookupGranularity;
    return detail::kIndexLookup[lookup_index];
}

std::size_t IndexToSize(std::size_t index) {
    assert(index < kNumSizeClasses);
    return kSizeClasses[index];
}

std::size_t NumToMove(std::size_t index) {
    const std::size_t object_size = IndexToSize(index);

    // 小对象尽量多搬运，大对象减少搬运次数，避免线程缓存占用过多内存。
    const std::size_t desired =
        std::max<std::size_t>(1, (64 * 1024) / object_size);
    return std::min<std::size_t>(kBatchSize, desired);
}

}  // namespace my_alloc
