// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Util/BlockAllocator.cpp @ v1.20.4 (GPL-2.0-or-later).
// The intrusive doubly linked list became a std::list; the search order and
// the way blocks are split and merged are unchanged.

#include "kernel/block_allocator.hpp"

namespace mpo::kernel {

void BlockAllocator::init(std::uint32_t range_start, std::uint32_t range_size) {
    shutdown();
    range_start_ = range_start;
    range_size_ = range_size;
    blocks_.push_back(Block{range_start, range_size, false, "(untitled)"});
}

void BlockAllocator::shutdown() {
    blocks_.clear();
}

void BlockAllocator::set_allocated(Block &block, const char *tag) {
    block.taken = true;
    block.tag = tag != nullptr ? tag : "---";
}

std::uint32_t BlockAllocator::alloc_aligned(std::uint32_t &size, std::uint32_t size_grain,
                                            std::uint32_t grain, bool from_top, const char *tag) {
    if (size == 0 || size > range_size_) return kFailed;

    // It could be off step, but the grain should generally be a power of 2.
    if (grain < grain_) grain = grain_;
    if (size_grain < grain_) size_grain = grain_;
    size = (size + size_grain - 1) & ~(size_grain - 1);

    if (!from_top) {
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            Block &b = *it;
            std::uint32_t offset = b.start % grain;
            if (offset != 0) offset = grain - offset;
            const std::uint32_t needed = offset + size;
            if (!b.taken && b.size >= needed) {
                if (b.size != needed) insert_free_after(it, b.size - needed);
                if (offset >= grain_) insert_free_before(it, offset);
                set_allocated(b, tag);
                return b.start;
            }
        }
    } else {
        for (auto it = blocks_.end(); it != blocks_.begin();) {
            --it;
            Block &b = *it;
            const std::uint32_t offset = (b.start + b.size - size) % grain;
            const std::uint32_t needed = offset + size;
            if (!b.taken && b.size >= needed) {
                if (b.size != needed) insert_free_before(it, b.size - needed);
                if (offset >= grain_) insert_free_after(it, offset);
                set_allocated(b, tag);
                return b.start;
            }
        }
    }
    return kFailed;
}

std::uint32_t BlockAllocator::alloc(std::uint32_t &size, bool from_top, const char *tag) {
    // Aligned in case alloc_at() left a block off the grain.
    return alloc_aligned(size, grain_, grain_, from_top, tag);
}

std::uint32_t BlockAllocator::alloc_at(std::uint32_t position, std::uint32_t size,
                                       const char *tag) {
    if (size > range_size_) return kFailed;

    // Downalign the position so whole blocks are allocated.
    std::uint32_t aligned_position = position;
    std::uint32_t aligned_size = size;
    if ((position & (grain_ - 1)) != 0) {
        aligned_position &= ~(grain_ - 1);
        aligned_size += position - aligned_position;
    }
    aligned_size = (aligned_size + grain_ - 1) & ~(grain_ - 1);

    auto it = block_from_address(aligned_position);
    if (it == blocks_.end()) return kFailed;
    Block &b = *it;
    if (b.taken) return kFailed;
    if (b.start + b.size < aligned_position + aligned_size) return kFailed;
    if (b.start == aligned_position) {
        if (b.size != aligned_size) insert_free_after(it, b.size - aligned_size);
    } else {
        insert_free_before(it, aligned_position - b.start);
        if (b.size > aligned_size) insert_free_after(it, b.size - aligned_size);
    }
    set_allocated(b, tag);
    return position;
}

void BlockAllocator::merge_free_blocks(Iterator from) {
    while (from != blocks_.begin()) {
        auto prev = std::prev(from);
        if (prev->taken) break;
        prev->size += from->size;
        blocks_.erase(from);
        from = prev;
    }
    for (auto next = std::next(from); next != blocks_.end() && !next->taken;
         next = std::next(from)) {
        from->size += next->size;
        blocks_.erase(next);
    }
}

bool BlockAllocator::free(std::uint32_t position) {
    auto it = block_from_address(position);
    if (it == blocks_.end() || !it->taken) return false;
    it->taken = false;
    merge_free_blocks(it);
    return true;
}

bool BlockAllocator::free_exact(std::uint32_t position) {
    auto it = block_from_address(position);
    if (it == blocks_.end() || !it->taken || it->start != position) return false;
    it->taken = false;
    merge_free_blocks(it);
    return true;
}

void BlockAllocator::insert_free_before(Iterator block, std::uint32_t size) {
    blocks_.insert(block, Block{block->start, size, false, "(untitled)"});
    block->start += size;
    block->size -= size;
}

void BlockAllocator::insert_free_after(Iterator block, std::uint32_t size) {
    blocks_.insert(std::next(block),
                   Block{block->start + block->size - size, size, false, "(untitled)"});
    block->size -= size;
}

BlockAllocator::Iterator BlockAllocator::block_from_address(std::uint32_t address) {
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->start <= address && it->start + it->size > address) return it;
    }
    return blocks_.end();
}

const BlockAllocator::Block *BlockAllocator::block_from_address_const(std::uint32_t address) const {
    for (const Block &b : blocks_) {
        if (b.start <= address && b.start + b.size > address) return &b;
    }
    return nullptr;
}

std::uint32_t BlockAllocator::block_start_from_address(std::uint32_t address) const {
    const Block *b = block_from_address_const(address);
    return b != nullptr ? b->start : kFailed;
}

std::uint32_t BlockAllocator::block_size_from_address(std::uint32_t address) const {
    const Block *b = block_from_address_const(address);
    return b != nullptr ? b->size : kFailed;
}

std::uint32_t BlockAllocator::largest_free_block_size() const {
    std::uint32_t largest = 0;
    for (const Block &b : blocks_) {
        if (!b.taken && b.size > largest) largest = b.size;
    }
    return largest;
}

std::uint32_t BlockAllocator::total_free_bytes() const {
    std::uint32_t sum = 0;
    for (const Block &b : blocks_) {
        if (!b.taken) sum += b.size;
    }
    return sum;
}

std::vector<BlockAllocator::BlockInfo> BlockAllocator::blocks() const {
    std::vector<BlockInfo> out;
    out.reserve(blocks_.size());
    for (const Block &b : blocks_) out.push_back(BlockInfo{b.start, b.size, b.taken, b.tag});
    return out;
}

} // namespace mpo::kernel
