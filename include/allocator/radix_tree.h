#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

#include "span.h"

namespace my_alloc {

class RadixTree {
public:
    RadixTree();
    ~RadixTree();

    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;

    Span* GetSpan(const void* ptr) const;
    void SetSpan(void* ptr, Span* span);
    void ClearSpan(void* ptr);

    // 便于将一个 Span 覆盖的所有页统一写入映射表。
    void SetSpanRange(Span* span);
    void ClearSpanRange(const Span& span);

private:
    static constexpr std::size_t kLevelBits = 9;
    static constexpr std::size_t kFanout = 1ULL << kLevelBits;
    static constexpr std::size_t kLevelMask = kFanout - 1;

    struct LeafNode {
        std::array<std::atomic<Span*>, kFanout> slots{};

        LeafNode();
    };

    struct MidNode {
        std::array<std::atomic<LeafNode*>, kFanout> children{};

        MidNode();
    };

    struct TopNode {
        std::array<std::atomic<MidNode*>, kFanout> children{};

        TopNode();
    };

    std::array<std::atomic<TopNode*>, kFanout> root_{};
    mutable std::mutex write_lock_;

    static std::size_t Level0(PageId page_id);
    static std::size_t Level1(PageId page_id);
    static std::size_t Level2(PageId page_id);
    static std::size_t Level3(PageId page_id);

    static void DestroyTopNode(TopNode* node);
    static void DestroyMidNode(MidNode* node);
};

}  // namespace my_alloc
