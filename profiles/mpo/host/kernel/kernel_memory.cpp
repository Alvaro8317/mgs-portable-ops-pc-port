// SPDX-License-Identifier: GPL-3.0-or-later
//
// SysMem partitions and blocks, fixed-length (FPL) and variable-length (VPL)
// pools, SDK version.
//
// Ported from PPSSPP Core/HLE/sceKernelMemory.cpp and sceKernelMemory.h @
// v1.20.4 (GPL-2.0-or-later). The VPL keeps PPSSPP's SceKernelVplHeader in
// guest memory, so allocations land on the same addresses.

#include "kernel/kernel_impl.hpp"

#include <algorithm>

namespace mpo::kernel {

namespace {

constexpr std::uint32_t kPoolAttrPriority = 0x0100;
constexpr std::uint32_t kPoolAttrHighMem = 0x4000;
constexpr std::uint32_t kFplAttrKnown = kPoolAttrPriority | kPoolAttrHighMem;
constexpr std::uint32_t kVplAttrSmallest = 0x0200;
constexpr std::uint32_t kVplAttrMaskOrder = 0x0300;
constexpr std::uint32_t kVplAttrKnown = kPoolAttrPriority | kVplAttrSmallest | kPoolAttrHighMem;

bool pool_sort_priority(const Kernel::Impl &k, const PoolWaiter &a, const PoolWaiter &b) {
    const Thread *ta = k.thread(a.thread);
    const Thread *tb = k.thread(b.thread);
    return (ta != nullptr ? ta->current_priority : 0) < (tb != nullptr ? tb->current_priority : 0);
}

std::uint32_t pool_timeout_us(std::int32_t micro) {
    // How the hardware seems to time things. Yes, 7 is special.
    if (micro <= 5) return 20;
    if (micro == 7) return 25;
    if (micro <= 215) return 250;
    return static_cast<std::uint32_t>(micro);
}

} // namespace

BlockAllocator *Kernel::Impl::allocator_from_id(std::uint32_t partition) {
    switch (partition) {
    case 1:
    case 3:
    case 4:
        return kernel_mode ? &kernel_memory : nullptr;
    case 2:
    case 6:
        return &user_memory;
    case 8:
    case 10:
        return kernel_mode ? &user_memory : nullptr;
    case 5:
        return &volatile_memory;
    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Partition blocks.

std::uint32_t Kernel::alloc_partition_memory(std::uint32_t partition, const char *name,
                                             std::uint32_t type, std::uint32_t size,
                                             std::uint32_t addr) {
    Impl &k = *impl_;
    if (type > memblock_type::kHighAligned) return error::kIllegalMemblockType;
    // Alignment is only allowed for powers of 2.
    if (type == memblock_type::kLowAligned || type == memblock_type::kHighAligned) {
        if ((addr & (addr - 1)) != 0 || addr == 0) return error::kIllegalAlignmentSize;
    }
    if (partition < 1 || partition > 9 || partition == 7) return error::kIllegalArgument;
    BlockAllocator *allocator = k.allocator_from_id(partition);
    if (allocator == nullptr) return error::kIllegalPartition;
    if (name == nullptr) return error::kError;
    if (size == 0) return error::kMemblockAllocFailed;

    const std::string tag = Impl::clip_name(name);
    std::uint32_t address = BlockAllocator::kFailed;
    std::uint32_t rounded = size;
    if (type == memblock_type::kAddr) {
        address = allocator->alloc_at(addr & ~0xFFu, size, tag.c_str());
    } else if (type == memblock_type::kLowAligned || type == memblock_type::kHighAligned) {
        address = allocator->alloc_aligned(rounded, 0x100, addr,
                                           type == memblock_type::kHighAligned, tag.c_str());
    } else {
        address = allocator->alloc(rounded, type == memblock_type::kHigh, tag.c_str());
    }
    if (address == BlockAllocator::kFailed) return error::kMemblockAllocFailed;
    auto block = std::make_unique<PartitionBlock>();
    block->name = tag;
    block->allocator = allocator;
    block->address = address;
    return static_cast<std::uint32_t>(k.create_object(std::move(block)));
}

std::uint32_t Kernel::free_partition_memory(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    PartitionBlock *block = k.get<PartitionBlock>(uid, error);
    if (block == nullptr) return error;
    block->allocator->free(block->address);
    return k.destroy_object(uid, ObjType::kPartitionBlock, error::kUnknownUid);
}

std::uint32_t Kernel::get_block_head_addr(Uid uid) {
    PartitionBlock *block = impl_->get<PartitionBlock>(uid);
    return block != nullptr ? block->address : 0u;
}

std::uint32_t Kernel::max_free_mem_size() {
    return impl_->user_memory.largest_free_block_size();
}

std::uint32_t Kernel::total_free_mem_size() {
    return impl_->user_memory.total_free_bytes();
}

std::uint32_t Kernel::set_compiled_sdk_version(std::uint32_t version) {
    impl_->sdk_version = version;
    impl_->has_sdk_version = true;
    return 0;
}

std::uint32_t Kernel::set_compiler_version(std::uint32_t version) {
    impl_->compiler_version = version;
    return 0;
}

std::uint32_t Kernel::compiled_sdk_version() const {
    return impl_->has_sdk_version ? impl_->sdk_version : 0u;
}

BlockAllocator &Kernel::user_memory() {
    return impl_->user_memory;
}

BlockAllocator &Kernel::kernel_memory() {
    return impl_->kernel_memory;
}

BlockAllocator &Kernel::volatile_memory() {
    return impl_->volatile_memory;
}

void Kernel::set_kernel_mode(bool kernel_mode) {
    impl_->kernel_mode = kernel_mode;
}

// ---------------------------------------------------------------------------
// FPL.

int Kernel::Impl::fpl_allocate_block(Fpl &fpl) {
    for (int i = 0; i < fpl.num_blocks; ++i) {
        const int b = fpl.next_block++ % fpl.num_blocks;
        if (!fpl.blocks[static_cast<std::size_t>(b)]) {
            fpl.blocks[static_cast<std::size_t>(b)] = true;
            return b;
        }
    }
    return -1;
}

bool Kernel::Impl::unlock_fpl_for_thread(Fpl &fpl, PoolWaiter &waiter, std::uint32_t result,
                                         bool &woke) {
    if (!verify_wait(waiter.thread, WaitType::kFpl, fpl.uid)) return true;
    if (result == 0) {
        const int block = fpl_allocate_block(fpl);
        if (block < 0) return false;
        mem.write_u32(waiter.addr_ptr,
                      fpl.address + static_cast<std::uint32_t>(fpl.aligned_size * block));
    }
    const std::uint32_t timeout_ptr = thread(waiter.thread)->timeout_ptr;
    if (timeout_ptr != 0)
        write_timeout_left(timeout_ptr, unschedule(TimerKind::kFplTimeout, waiter.thread));
    resume_from_wait(waiter.thread, result);
    woke = true;
    return true;
}

bool Kernel::Impl::clear_fpl_threads(Fpl &fpl, std::uint32_t reason) {
    bool woke = false;
    std::vector<PoolWaiter> waiting = fpl.waiting;
    for (auto &waiter : waiting) unlock_fpl_for_thread(fpl, waiter, reason, woke);
    fpl.waiting.clear();
    return woke;
}

void Kernel::Impl::sort_fpl_threads(Fpl &fpl) {
    cleanup_waiting(*this, WaitType::kFpl, fpl.uid, fpl.waiting);
    if ((fpl.attr & kPoolAttrPriority) != 0) {
        std::stable_sort(fpl.waiting.begin(), fpl.waiting.end(),
                         [this](const PoolWaiter &a, const PoolWaiter &b) {
                             return pool_sort_priority(*this, a, b);
                         });
    }
}

void Kernel::Impl::set_fpl_timeout(std::uint32_t timeout_ptr) {
    if (timeout_ptr == 0 || current == nullptr) return;
    const auto micro = static_cast<std::int32_t>(mem.read_u32(timeout_ptr));
    schedule(TimerKind::kFplTimeout, current->uid, pool_timeout_us(micro));
}

std::uint32_t Kernel::create_fpl(const char *name, std::uint32_t partition, std::uint32_t attr,
                                 std::uint32_t block_size, std::uint32_t num_blocks,
                                 std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kNoMemory;
    if (partition < 1 || partition > 9 || partition == 7) return error::kIllegalArgument;
    BlockAllocator *allocator = k.allocator_from_id(partition);
    if (allocator == nullptr) return error::kIllegalPerm;
    if (((attr & ~kFplAttrKnown) & ~0xFFu) != 0) return error::kIllegalAttr;

    // Based on results from a PSP.
    bool illegal = block_size == 0 || num_blocks == 0;
    if (!illegal && static_cast<std::uint64_t>(block_size) > (0x100000000ULL / num_blocks) - 4ULL)
        illegal = true;
    if (!illegal &&
        static_cast<std::uint64_t>(num_blocks) >=
            0x100000000ULL / ((static_cast<std::uint64_t>(block_size) + 3ULL) & ~3ULL)) {
        illegal = true;
    }
    if (illegal) return error::kIllegalMemsize;

    std::uint32_t alignment = 4;
    if (k.mem.is_valid(option_addr, 4)) {
        const std::uint32_t size = k.mem.read_u32(option_addr);
        if (size >= 4) alignment = k.mem.read_u32(option_addr + 4);
        if ((alignment & (alignment - 1)) != 0) return error::kIllegalArgument;
    }
    if (static_cast<std::int32_t>(alignment) < 4) alignment = 4;
    const std::uint32_t aligned_size = (block_size + alignment - 1) & ~(alignment - 1);
    std::uint32_t total = aligned_size * num_blocks;
    const std::string tag = "FPL/" + Impl::clip_name(name);
    const std::uint32_t address =
        allocator->alloc(total, (attr & kPoolAttrHighMem) != 0, tag.c_str());
    if (address == BlockAllocator::kFailed) return error::kNoMemory;

    auto fpl = std::make_unique<Fpl>();
    fpl->name = Impl::clip_name(name);
    fpl->attr = attr;
    fpl->block_size = static_cast<std::int32_t>(block_size);
    fpl->num_blocks = static_cast<std::int32_t>(num_blocks);
    fpl->num_free_blocks = static_cast<std::int32_t>(num_blocks);
    fpl->blocks.assign(num_blocks, false);
    fpl->address = address;
    fpl->aligned_size = static_cast<std::int32_t>(aligned_size);
    fpl->allocator = allocator;
    return static_cast<std::uint32_t>(k.create_object(std::move(fpl)));
}

std::uint32_t Kernel::delete_fpl(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Fpl *fpl = k.get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    CallScope call(k);
    if (k.clear_fpl_threads(*fpl, error::kWaitDelete)) k.request_resched();
    fpl->allocator->free(fpl->address);
    return call.finish(k.destroy_object(uid, ObjType::kFpl, error::kUnknownFplid));
}

std::uint32_t Kernel::Impl::allocate_fpl_call(Uid uid, std::uint32_t block_ptr_addr,
                                              std::uint32_t timeout_ptr, bool callbacks) {
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    std::uint32_t error = 0;
    Fpl *fpl = get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    const int block = fpl_allocate_block(*fpl);
    if (block >= 0) {
        mem.write_u32(block_ptr_addr,
                      fpl->address + static_cast<std::uint32_t>(fpl->aligned_size * block));
        return 0;
    }
    if (!on_current_fiber()) return error::kIllegalContext;
    CallScope call(*this);
    const Uid cur = current->uid;
    fpl->waiting.erase(std::remove_if(fpl->waiting.begin(), fpl->waiting.end(),
                                      [cur](const PoolWaiter &w) { return w.thread == cur; }),
                       fpl->waiting.end());
    fpl->waiting.push_back(PoolWaiter{cur, block_ptr_addr});
    set_fpl_timeout(timeout_ptr);
    wait_current(WaitType::kFpl, uid, 0, timeout_ptr, callbacks);
    return call.finish(0);
}

std::uint32_t Kernel::allocate_fpl(Uid uid, std::uint32_t block_addr_out,
                                   std::uint32_t timeout_addr) {
    return impl_->allocate_fpl_call(uid, block_addr_out, timeout_addr, false);
}

std::uint32_t Kernel::allocate_fpl_cb(Uid uid, std::uint32_t block_addr_out,
                                      std::uint32_t timeout_addr) {
    return impl_->allocate_fpl_call(uid, block_addr_out, timeout_addr, true);
}

std::uint32_t Kernel::try_allocate_fpl(Uid uid, std::uint32_t block_addr_out) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Fpl *fpl = k.get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    const int block = k.fpl_allocate_block(*fpl);
    if (block < 0) return error::kNoMemory;
    k.mem.write_u32(block_addr_out,
                    fpl->address + static_cast<std::uint32_t>(fpl->aligned_size * block));
    return 0;
}

std::uint32_t Kernel::free_fpl(Uid uid, std::uint32_t block_addr) {
    Impl &k = *impl_;
    if (block_addr > k.config.user_memory_end) return error::kIllegalAddr;
    std::uint32_t error = 0;
    Fpl *fpl = k.get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    const auto block = static_cast<std::int32_t>((block_addr - fpl->address) /
                                                 static_cast<std::uint32_t>(fpl->aligned_size));
    if (block < 0 || block >= fpl->num_blocks) return error::kIllegalMemblock;
    if (!fpl->blocks[static_cast<std::size_t>(block)])
        return error::kIllegalMemblock; // Already free.
    fpl->blocks[static_cast<std::size_t>(block)] = false;
    CallScope call(k);
    k.sort_fpl_threads(*fpl);
    bool woke = false;
    for (std::size_t i = 0; i < fpl->waiting.size();) {
        PoolWaiter waiter = fpl->waiting[i];
        if (k.unlock_fpl_for_thread(*fpl, waiter, 0, woke)) {
            fpl->waiting.erase(fpl->waiting.begin() + static_cast<std::ptrdiff_t>(i));
            i = 0;
        } else {
            ++i;
        }
    }
    if (woke) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::cancel_fpl(Uid uid, std::uint32_t num_wait_threads_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Fpl *fpl = k.get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    CallScope call(k);
    fpl->num_wait_threads = static_cast<std::int32_t>(fpl->waiting.size());
    if (k.mem.is_valid(num_wait_threads_addr)) {
        k.mem.write_u32(num_wait_threads_addr, static_cast<std::uint32_t>(fpl->num_wait_threads));
    }
    if (k.clear_fpl_threads(*fpl, error::kWaitCancel)) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::refer_fpl_status(Uid uid, std::uint32_t status_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Fpl *fpl = k.get<Fpl>(uid, error);
    if (fpl == nullptr) return error;
    k.sort_fpl_threads(*fpl);
    fpl->num_wait_threads = static_cast<std::int32_t>(fpl->waiting.size());
    fpl->num_free_blocks =
        static_cast<std::int32_t>(std::count(fpl->blocks.begin(), fpl->blocks.end(), false));
    if (k.mem.is_valid(status_addr, 56) && k.mem.read_u32(status_addr) != 0) {
        k.mem.write_u32(status_addr, 56);
        k.write_name(status_addr + 4, fpl->name);
        k.mem.write_u32(status_addr + 36, fpl->attr);
        k.mem.write_u32(status_addr + 40, static_cast<std::uint32_t>(fpl->block_size));
        k.mem.write_u32(status_addr + 44, static_cast<std::uint32_t>(fpl->num_blocks));
        k.mem.write_u32(status_addr + 48, static_cast<std::uint32_t>(fpl->num_free_blocks));
        k.mem.write_u32(status_addr + 52, static_cast<std::uint32_t>(fpl->num_wait_threads));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// VPL header (SceKernelVplHeader) in guest memory:
//   +0x00 startPtr  +0x04 startPtr2  +0x08 sentinel  +0x0C sizeMinus8
//   +0x10 allocatedInBlocks  +0x14 nextFreeBlock  +0x18 firstBlock {next, sizeInBlocks}
// A block is {next, sizeInBlocks}, 8 bytes, and sizes count 8-byte units.

namespace {

struct VplView {
    GuestMemory &mem;
    std::uint32_t header;

    std::uint32_t start() const { return mem.read_u32(header); }
    std::uint32_t size_minus8() const { return mem.read_u32(header + 0x0C); }
    std::uint32_t allocated() const { return mem.read_u32(header + 0x10); }
    void set_allocated(std::uint32_t v) { mem.write_u32(header + 0x10, v); }
    std::uint32_t next_free() const { return mem.read_u32(header + 0x14); }
    void set_next_free(std::uint32_t v) { mem.write_u32(header + 0x14, v); }
    std::uint32_t first_block_ptr() const { return start() + 0x18; }
    std::uint32_t last_block_ptr() const { return start() + size_minus8(); }
    std::uint32_t sentinel_ptr() const { return start() + 8; }

    std::uint32_t next(std::uint32_t b) const { return mem.read_u32(b); }
    void set_next(std::uint32_t b, std::uint32_t v) { mem.write_u32(b, v); }
    std::uint32_t blocks(std::uint32_t b) const { return mem.read_u32(b + 4); }
    void set_blocks(std::uint32_t b, std::uint32_t v) { mem.write_u32(b + 4, v); }
    bool valid(std::uint32_t b) const { return mem.is_valid(b); }

    std::uint32_t split(std::uint32_t b, std::uint32_t alloc_blocks) {
        const std::uint32_t prev = b;
        set_blocks(b, blocks(b) - alloc_blocks);
        b += blocks(b) * 8;
        set_blocks(b, alloc_blocks);
        set_next(b, prev);
        return b;
    }
    void unlink(std::uint32_t b, std::uint32_t prev) {
        set_allocated(allocated() + blocks(b));
        set_next(prev, next(b));
        set_next_free(prev);
        set_next(b, sentinel_ptr());
    }
    std::uint32_t merge(std::uint32_t first, std::uint32_t second) {
        set_blocks(first, blocks(first) + blocks(second));
        set_next(first, next(second));
        return first;
    }
    void link(std::uint32_t b, std::uint32_t prev, std::uint32_t nxt) {
        set_allocated(allocated() - blocks(b));
        set_next_free(prev);
        // Erase the magic so it isn't considered allocated later.
        set_next(b, nxt);
        if (b + blocks(b) * 8 == nxt && blocks(nxt) != 0) b = merge(b, nxt);
        if (prev + blocks(prev) * 8 == b) {
            merge(prev, b);
        } else {
            set_next(prev, b);
        }
    }
};

} // namespace

void Kernel::Impl::vpl_header_init(std::uint32_t header, std::uint32_t size) {
    VplView v{mem, header};
    mem.write_u32(header, header);
    mem.write_u32(header + 0x04, header);
    mem.write_u32(header + 0x08, header + 7);
    mem.write_u32(header + 0x0C, size - 8);
    v.set_allocated(0);
    v.set_next_free(v.first_block_ptr());
    v.set_next(v.first_block_ptr(), v.last_block_ptr());
    // Includes its own header, which is one block.
    v.set_blocks(v.first_block_ptr(), (size - 0x28) / 8 + 1);
    v.set_next(v.last_block_ptr(), v.first_block_ptr());
    v.set_blocks(v.last_block_ptr(), 0);
}

std::uint32_t Kernel::Impl::vpl_header_allocate(std::uint32_t header, std::uint32_t size) {
    VplView v{mem, header};
    const std::uint32_t alloc_blocks = (size + 7) / 8 + 1;
    if (!v.valid(v.next_free())) return BlockAllocator::kFailed;
    const std::uint32_t start = v.next_free();
    std::uint32_t prev = start;
    do {
        std::uint32_t b = v.next(prev);
        if (v.blocks(b) > alloc_blocks) b = v.split(b, alloc_blocks);
        if (v.blocks(b) == alloc_blocks) {
            v.unlink(b, prev);
            return b + 8;
        }
        prev = b;
    } while (v.valid(prev) && prev != start);
    return BlockAllocator::kFailed;
}

bool Kernel::Impl::vpl_header_free(std::uint32_t header, std::uint32_t ptr) {
    VplView v{mem, header};
    const std::uint32_t b = ptr - 8;
    // In range? It can't be the last block, which is always 0.
    if (!v.valid(b) || ptr < v.first_block_ptr() || ptr >= v.last_block_ptr()) return false;
    // Does it carry the allocated magic?
    if (v.next(b) != v.sentinel_ptr() || v.blocks(b) > v.allocated()) return false;
    const std::uint32_t last = v.last_block_ptr();
    std::uint32_t prev = last;
    do {
        const std::uint32_t nxt = v.next(prev);
        if (nxt == b) return false; // Already free.
        if (nxt > b) {
            v.link(b, prev, nxt);
            return true;
        }
        prev = nxt;
    } while (v.valid(prev) && prev != last);
    return false;
}

std::uint32_t Kernel::Impl::vpl_header_free_size(std::uint32_t header) {
    VplView v{mem, header};
    // Size less the header and the allocated bytes.
    return v.size_minus8() + 8 - 0x20 - v.allocated() * 8;
}

// ---------------------------------------------------------------------------
// VPL.

bool Kernel::Impl::unlock_vpl_for_thread(Vpl &vpl, PoolWaiter &waiter, std::uint32_t result,
                                         bool &woke) {
    if (!verify_wait(waiter.thread, WaitType::kVpl, vpl.uid)) return true;
    Thread *t = thread(waiter.thread);
    if (result == 0) {
        const std::uint32_t addr = vpl_header_allocate(vpl.header, t->wait_value);
        if (addr == BlockAllocator::kFailed) return false;
        mem.write_u32(waiter.addr_ptr, addr);
    }
    if (t->timeout_ptr != 0)
        write_timeout_left(t->timeout_ptr, unschedule(TimerKind::kVplTimeout, waiter.thread));
    resume_from_wait(waiter.thread, result);
    woke = true;
    return true;
}

bool Kernel::Impl::clear_vpl_threads(Vpl &vpl, std::uint32_t reason) {
    bool woke = false;
    std::vector<PoolWaiter> waiting = vpl.waiting;
    for (auto &waiter : waiting) unlock_vpl_for_thread(vpl, waiter, reason, woke);
    vpl.waiting.clear();
    return woke;
}

void Kernel::Impl::sort_vpl_threads(Vpl &vpl) {
    cleanup_waiting(*this, WaitType::kVpl, vpl.uid, vpl.waiting);
    if ((vpl.attr & kPoolAttrPriority) != 0) {
        std::stable_sort(vpl.waiting.begin(), vpl.waiting.end(),
                         [this](const PoolWaiter &a, const PoolWaiter &b) {
                             return pool_sort_priority(*this, a, b);
                         });
    }
}

void Kernel::Impl::vpl_timeout(Uid thread_uid) {
    const Thread *t = thread(thread_uid);
    const Uid uid = t != nullptr ? t->wait_id_for(WaitType::kVpl) : 0;
    wait_exec_timeout<Vpl>(thread_uid, WaitType::kVpl);
    // In FIFO mode, that may have cleared another thread to wake up.
    Vpl *vpl = get<Vpl>(uid);
    if (vpl != nullptr && (vpl->attr & kVplAttrMaskOrder) == 0) {
        bool woke = false;
        while (!vpl->waiting.empty()) {
            PoolWaiter waiter = vpl->waiting.front();
            if (!unlock_vpl_for_thread(*vpl, waiter, 0, woke)) break;
            vpl->waiting.erase(vpl->waiting.begin());
        }
    }
}

std::uint32_t Kernel::create_vpl(const char *name, std::uint32_t partition, std::uint32_t attr,
                                 std::uint32_t size, std::uint32_t option_addr) {
    Impl &k = *impl_;
    if (name == nullptr) return error::kError;
    if (partition < 1 || partition > 9 || partition == 7) return error::kIllegalArgument;
    BlockAllocator *allocator = k.allocator_from_id(partition);
    if (allocator == nullptr) return error::kIllegalPerm;
    if (((attr & ~kVplAttrKnown) & ~0xFFu) != 0) return error::kIllegalAttr;
    if (size == 0) return error::kIllegalMemsize;
    // The block allocator would accept this; stop it here.
    if (size >= 0x80000000u) return error::kNoMemory;
    // Can't have that little space in a VPL.
    if (size <= 0x30) size = 0x1000;
    size = (size + 7) & ~7u;
    // The allocator rounds to 256; the pool keeps the 8-aligned size.
    std::uint32_t alloc_size = size;
    const std::string tag = "VPL/" + Impl::clip_name(name);
    const std::uint32_t block =
        allocator->alloc(alloc_size, (attr & kPoolAttrHighMem) != 0, tag.c_str());
    if (block == BlockAllocator::kFailed) return error::kNoMemory;

    auto vpl = std::make_unique<Vpl>();
    vpl->name = Impl::clip_name(name);
    vpl->attr = attr;
    vpl->pool_size = static_cast<std::int32_t>(size - 0x20);
    vpl->free_size = vpl->pool_size;
    // The first 32 bytes are the pool's bookkeeping.
    vpl->address = block + 0x20;
    vpl->header = block;
    vpl->allocator = allocator;
    k.vpl_header_init(block, size);
    (void)option_addr;
    return static_cast<std::uint32_t>(k.create_object(std::move(vpl)));
}

std::uint32_t Kernel::delete_vpl(Uid uid) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Vpl *vpl = k.get<Vpl>(uid, error);
    if (vpl == nullptr) return error;
    CallScope call(k);
    if (k.clear_vpl_threads(*vpl, error::kWaitDelete)) k.request_resched();
    vpl->allocator->free(vpl->address);
    k.destroy_object(uid, ObjType::kVpl, error::kUnknownVplid);
    return call.finish(0);
}

bool Kernel::Impl::allocate_vpl_impl(Uid uid, std::uint32_t size, std::uint32_t addr_ptr,
                                     std::uint32_t &error, bool trying) {
    Vpl *vpl = get<Vpl>(uid, error);
    if (vpl == nullptr) return false;
    if (size == 0 || size > static_cast<std::uint32_t>(vpl->pool_size)) {
        error = error::kIllegalMemsize;
        return false;
    }
    // For some reason, try doesn't follow the same rules.
    if (!trying && (vpl->attr & kVplAttrMaskOrder) == 0) {
        sort_vpl_threads(*vpl);
        if (!vpl->waiting.empty()) {
            // Blocked by the FIFO queue.
            error = error::kNoMemory;
            return true;
        }
    }
    const std::uint32_t addr = vpl_header_allocate(vpl->header, size);
    if (addr != BlockAllocator::kFailed) {
        mem.write_u32(addr_ptr, addr);
        error = 0;
    } else {
        error = error::kNoMemory;
    }
    return true;
}

std::uint32_t Kernel::Impl::allocate_vpl_call(Uid uid, std::uint32_t size, std::uint32_t addr_ptr,
                                              std::uint32_t timeout_ptr, bool callbacks) {
    if (!dispatch_allowed()) return error::kCanNotWait;
    if (in_interrupt) return error::kIllegalContext;
    std::uint32_t error = 0;
    if (!allocate_vpl_impl(uid, size, addr_ptr, error, false)) return error;
    CallScope call(*this);
    if (callbacks) request_current_callbacks();
    Vpl *vpl = get<Vpl>(uid);
    if (error == error::kNoMemory) {
        if (timeout_ptr != 0 && mem.read_u32(timeout_ptr) == 0)
            return call.finish(error::kWaitTimeout);
        if (!on_current_fiber()) return call.finish(error::kIllegalContext);
        const Uid cur = current->uid;
        vpl->waiting.erase(std::remove_if(vpl->waiting.begin(), vpl->waiting.end(),
                                          [cur](const PoolWaiter &w) { return w.thread == cur; }),
                           vpl->waiting.end());
        vpl->waiting.push_back(PoolWaiter{cur, addr_ptr});
        if (timeout_ptr != 0) {
            const auto micro = static_cast<std::int32_t>(mem.read_u32(timeout_ptr));
            schedule(TimerKind::kVplTimeout, cur, pool_timeout_us(micro));
        }
        wait_current(WaitType::kVpl, uid, size, timeout_ptr, callbacks);
    } else if (error == 0 && !vpl->waiting.empty()) {
        // If anyone else was waiting, the allocation causes a delay.
        return call.finish(on_current_fiber() ? delay_result_impl(0, 50) : 0u);
    }
    return call.finish(error);
}

std::uint32_t Kernel::allocate_vpl(Uid uid, std::uint32_t size, std::uint32_t addr_out,
                                   std::uint32_t timeout_addr) {
    return impl_->allocate_vpl_call(uid, size, addr_out, timeout_addr, false);
}

std::uint32_t Kernel::allocate_vpl_cb(Uid uid, std::uint32_t size, std::uint32_t addr_out,
                                      std::uint32_t timeout_addr) {
    return impl_->allocate_vpl_call(uid, size, addr_out, timeout_addr, true);
}

std::uint32_t Kernel::try_allocate_vpl(Uid uid, std::uint32_t size, std::uint32_t addr_out) {
    std::uint32_t error = 0;
    impl_->allocate_vpl_impl(uid, size, addr_out, error, true);
    return error;
}

std::uint32_t Kernel::free_vpl(Uid uid, std::uint32_t addr) {
    Impl &k = *impl_;
    if (addr != 0 && !k.mem.is_valid(addr)) return error::kIllegalAddr;
    std::uint32_t error = 0;
    Vpl *vpl = k.get<Vpl>(uid, error);
    if (vpl == nullptr) return error;
    if (!k.vpl_header_free(vpl->header, addr)) return error::kIllegalMemblock;
    CallScope call(k);
    k.sort_vpl_threads(*vpl);
    bool woke = false;
    for (std::size_t i = 0; i < vpl->waiting.size();) {
        PoolWaiter waiter = vpl->waiting[i];
        if (k.unlock_vpl_for_thread(*vpl, waiter, 0, woke)) {
            vpl->waiting.erase(vpl->waiting.begin() + static_cast<std::ptrdiff_t>(i));
            i = 0;
        } else if ((vpl->attr & kVplAttrMaskOrder) == 0) {
            // In FIFO, stop at the first one that can't wake.
            break;
        } else {
            ++i;
        }
    }
    if (woke) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::cancel_vpl(Uid uid, std::uint32_t num_wait_threads_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Vpl *vpl = k.get<Vpl>(uid, error);
    if (vpl == nullptr) return error;
    CallScope call(k);
    vpl->num_wait_threads = static_cast<std::int32_t>(vpl->waiting.size());
    if (k.mem.is_valid(num_wait_threads_addr)) {
        k.mem.write_u32(num_wait_threads_addr, static_cast<std::uint32_t>(vpl->num_wait_threads));
    }
    if (k.clear_vpl_threads(*vpl, error::kWaitCancel)) k.request_resched();
    return call.finish(0);
}

std::uint32_t Kernel::refer_vpl_status(Uid uid, std::uint32_t info_addr) {
    Impl &k = *impl_;
    std::uint32_t error = 0;
    Vpl *vpl = k.get<Vpl>(uid, error);
    if (vpl == nullptr) return error;
    k.sort_vpl_threads(*vpl);
    vpl->num_wait_threads = static_cast<std::int32_t>(vpl->waiting.size());
    vpl->free_size = static_cast<std::int32_t>(k.vpl_header_free_size(vpl->header));
    if (k.mem.is_valid(info_addr, 52) && k.mem.read_u32(info_addr) != 0) {
        k.mem.write_u32(info_addr, 52);
        k.write_name(info_addr + 4, vpl->name);
        k.mem.write_u32(info_addr + 36, vpl->attr);
        k.mem.write_u32(info_addr + 40, static_cast<std::uint32_t>(vpl->pool_size));
        k.mem.write_u32(info_addr + 44, static_cast<std::uint32_t>(vpl->free_size));
        k.mem.write_u32(info_addr + 48, static_cast<std::uint32_t>(vpl->num_wait_threads));
    }
    return 0;
}

} // namespace mpo::kernel
