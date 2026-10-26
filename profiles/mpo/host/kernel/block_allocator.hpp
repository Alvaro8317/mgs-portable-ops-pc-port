// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/Util/BlockAllocator.h @ v1.20.4 (GPL-2.0-or-later).
//
// First-fit allocator over a guest address range, used for the SysMem
// partitions, thread stacks and FPL/VPL pools. Matching PPSSPP's placement
// rules (grain, from-top search, split order) is what makes the game get the
// same addresses as in PPSSPP (docs/analysis/mgp_main.md §7).

#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <vector>

namespace mpo::kernel {

class BlockAllocator {
public:
    static constexpr std::uint32_t kFailed = 0xFFFFFFFFu;

    explicit BlockAllocator(std::uint32_t grain = 16) : grain_(grain) {}

    void init(std::uint32_t range_start, std::uint32_t range_size);
    void shutdown();

    // `size` is rounded up to the grain and written back. Returns kFailed when
    // there is no room.
    std::uint32_t alloc(std::uint32_t &size, bool from_top = false, const char *tag = nullptr);
    std::uint32_t alloc_aligned(std::uint32_t &size, std::uint32_t size_grain, std::uint32_t grain,
                                bool from_top = false, const char *tag = nullptr);
    std::uint32_t alloc_at(std::uint32_t position, std::uint32_t size, const char *tag = nullptr);

    // Frees the block containing `position` (anywhere inside it).
    bool free(std::uint32_t position);
    // Frees only if `position` is exactly the start of a taken block.
    bool free_exact(std::uint32_t position);

    [[nodiscard]] std::uint32_t block_start_from_address(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t block_size_from_address(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t largest_free_block_size() const;
    [[nodiscard]] std::uint32_t total_free_bytes() const;
    [[nodiscard]] std::uint32_t range_start() const noexcept { return range_start_; }
    [[nodiscard]] std::uint32_t range_size() const noexcept { return range_size_; }

    struct BlockInfo {
        std::uint32_t start;
        std::uint32_t size;
        bool taken;
        std::string tag;
    };
    [[nodiscard]] std::vector<BlockInfo> blocks() const;

private:
    struct Block {
        std::uint32_t start;
        std::uint32_t size;
        bool taken;
        std::string tag;
    };
    using Iterator = std::list<Block>::iterator;

    Iterator block_from_address(std::uint32_t address);
    [[nodiscard]] const Block *block_from_address_const(std::uint32_t address) const;
    void insert_free_before(Iterator block, std::uint32_t size);
    void insert_free_after(Iterator block, std::uint32_t size);
    void merge_free_blocks(Iterator block);
    static void set_allocated(Block &block, const char *tag);

    std::list<Block> blocks_;
    std::uint32_t range_start_ = 0;
    std::uint32_t range_size_ = 0;
    std::uint32_t grain_;
};

} // namespace mpo::kernel
