#include "../include/allocator/radix_tree.h"

namespace fastalloc {

RadixTree::LeafNode::LeafNode() {
    for (auto& slot : slots) {
        slot.store(nullptr, std::memory_order_relaxed);
    }
}

RadixTree::MidNode::MidNode() {
    for (auto& child : children) {
        child.store(nullptr, std::memory_order_relaxed);
    }
}

RadixTree::TopNode::TopNode() {
    for (auto& child : children) {
        child.store(nullptr, std::memory_order_relaxed);
    }
}

RadixTree::RadixTree() {
    for (auto& node : root_) {
        node.store(nullptr, std::memory_order_relaxed);
    }
}

RadixTree::~RadixTree() {
    for (auto& node : root_) {
        DestroyTopNode(node.load(std::memory_order_acquire));
    }
}

Span* RadixTree::GetSpan(const void* ptr) const {
    const PageId page_id = PtrToPageId(ptr);

    TopNode* top = root_[Level0(page_id)].load(std::memory_order_acquire);
    if (top == nullptr) {
        return nullptr;
    }

    MidNode* mid = top->children[Level1(page_id)].load(std::memory_order_acquire);
    if (mid == nullptr) {
        return nullptr;
    }

    LeafNode* leaf =
        mid->children[Level2(page_id)].load(std::memory_order_acquire);
    if (leaf == nullptr) {
        return nullptr;
    }

    return leaf->slots[Level3(page_id)].load(std::memory_order_acquire);
}

void RadixTree::SetSpan(void* ptr, Span* span) {
    const PageId page_id = PtrToPageId(ptr);
    const std::size_t l0 = Level0(page_id);
    const std::size_t l1 = Level1(page_id);
    const std::size_t l2 = Level2(page_id);
    const std::size_t l3 = Level3(page_id);

    std::lock_guard<std::mutex> lock(write_lock_);

    TopNode* top = root_[l0].load(std::memory_order_acquire);
    if (top == nullptr) {
        top = new TopNode();
        root_[l0].store(top, std::memory_order_release);
    }

    MidNode* mid = top->children[l1].load(std::memory_order_acquire);
    if (mid == nullptr) {
        mid = new MidNode();
        top->children[l1].store(mid, std::memory_order_release);
    }

    LeafNode* leaf = mid->children[l2].load(std::memory_order_acquire);
    if (leaf == nullptr) {
        leaf = new LeafNode();
        mid->children[l2].store(leaf, std::memory_order_release);
    }

    leaf->slots[l3].store(span, std::memory_order_release);
}

void RadixTree::ClearSpan(void* ptr) {
    const PageId page_id = PtrToPageId(ptr);

    TopNode* top = root_[Level0(page_id)].load(std::memory_order_acquire);
    if (top == nullptr) {
        return;
    }

    MidNode* mid = top->children[Level1(page_id)].load(std::memory_order_acquire);
    if (mid == nullptr) {
        return;
    }

    LeafNode* leaf =
        mid->children[Level2(page_id)].load(std::memory_order_acquire);
    if (leaf == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(write_lock_);
    leaf->slots[Level3(page_id)].store(nullptr, std::memory_order_release);
}

void RadixTree::SetSpanRange(Span* span) {
    if (span == nullptr || span->page_num == 0) {
        return;
    }

    for (std::size_t i = 0; i < span->page_num; ++i) {
        SetSpan(PageIdToPtr(span->page_id + i), span);
    }
}

void RadixTree::ClearSpanRange(const Span& span) {
    if (span.page_num == 0) {
        return;
    }

    for (std::size_t i = 0; i < span.page_num; ++i) {
        ClearSpan(PageIdToPtr(span.page_id + i));
    }
}

std::size_t RadixTree::Level0(PageId page_id) {
    return (page_id >> (kLevelBits * 3)) & kLevelMask;
}

std::size_t RadixTree::Level1(PageId page_id) {
    return (page_id >> (kLevelBits * 2)) & kLevelMask;
}

std::size_t RadixTree::Level2(PageId page_id) {
    return (page_id >> kLevelBits) & kLevelMask;
}

std::size_t RadixTree::Level3(PageId page_id) {
    return page_id & kLevelMask;
}

void RadixTree::DestroyTopNode(TopNode* node) {
    if (node == nullptr) {
        return;
    }

    for (auto& child : node->children) {
        DestroyMidNode(child.load(std::memory_order_acquire));
    }
    delete node;
}

void RadixTree::DestroyMidNode(MidNode* node) {
    if (node == nullptr) {
        return;
    }

    for (auto& child : node->children) {
        delete child.load(std::memory_order_acquire);
    }
    delete node;
}

}  // namespace fastalloc
