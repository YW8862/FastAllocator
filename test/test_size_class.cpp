#include "../include/allocator/size_class.h"

#include <cassert>
#include <cstddef>

namespace {

using fastalloc::IndexToSize;
using fastalloc::NumToMove;
using fastalloc::SizeClassIndex;
using fastalloc::kMaxSmallObjectSize;
using fastalloc::kNumSizeClasses;

void TestAlignSize() {
    assert(fastalloc::AlignSize(0) == 0);
    assert(fastalloc::AlignSize(1) == 8);
    assert(fastalloc::AlignSize(8) == 8);
    assert(fastalloc::AlignSize(9) == 16);
    assert(fastalloc::AlignSize(16) == 16);
    assert(fastalloc::AlignSize(kMaxSmallObjectSize) == kMaxSmallObjectSize);
}

void TestMonotonicIndexToSize() {
    for (std::size_t index = 1; index < kNumSizeClasses; ++index) {
        assert(IndexToSize(index - 1) < IndexToSize(index));
    }
}

void TestCoverAllRequests() {
    for (std::size_t size = 1; size <= kMaxSmallObjectSize; ++size) {
        const std::size_t index = SizeClassIndex(size);
        const std::size_t class_size = IndexToSize(index);

        assert(index < kNumSizeClasses);
        assert(class_size >= size);
        if (index > 0) {
            assert(IndexToSize(index - 1) < size);
        }
    }
}

void TestTypicalSizes() {
    assert(IndexToSize(SizeClassIndex(8)) == 8);
    assert(IndexToSize(SizeClassIndex(16)) == 16);
    assert(IndexToSize(SizeClassIndex(100)) == 104);
    assert(IndexToSize(SizeClassIndex(1000)) == 1024);
    assert(IndexToSize(SizeClassIndex(256 * 1024)) == 256 * 1024);
}

void TestBoundarySizes() {
    assert(fastalloc::AlignSize(0) == 0);
    assert(fastalloc::AlignSize(1) == 8);
    assert(SizeClassIndex(0) == 0);
    assert(SizeClassIndex(1) == 0);
    assert(IndexToSize(SizeClassIndex(1)) == 8);

    const std::size_t max_small_index = SizeClassIndex(kMaxSmallObjectSize);
    assert(max_small_index == kNumSizeClasses - 1);
    assert(IndexToSize(max_small_index) == kMaxSmallObjectSize);

    const std::size_t large_index = SizeClassIndex(kMaxSmallObjectSize + 1);
    assert(large_index == kNumSizeClasses - 1);
    assert(IndexToSize(large_index) == kMaxSmallObjectSize);
}

void TestNumToMove() {
    const std::size_t tiny = NumToMove(SizeClassIndex(8));
    const std::size_t medium = NumToMove(SizeClassIndex(1024));
    const std::size_t large = NumToMove(SizeClassIndex(256 * 1024));

    assert(tiny >= medium);
    assert(medium >= large);
    assert(large >= 1);
}

}  // namespace

int main() {
    TestAlignSize();
    TestMonotonicIndexToSize();
    TestCoverAllRequests();
    TestTypicalSizes();
    TestBoundarySizes();
    TestNumToMove();
    return 0;
}
