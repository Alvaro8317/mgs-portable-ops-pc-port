// SPDX-License-Identifier: GPL-3.0-or-later

#include "kernel/fiber.hpp"

#include <minicoro.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MPO_FIBER_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MPO_FIBER_ASAN 1
#endif
#if defined(MPO_FIBER_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace mpo::kernel {

namespace {

std::size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<std::size_t>(size) : 4096u;
#endif
}

std::size_t round_up(std::size_t value, std::size_t grain) {
    return (value + grain - 1u) / grain * grain;
}

// minicoro puts its header, context and storage at the start of the block and
// the stack after them. Reserving with the OS (not malloc) keeps untouched
// stack pages free, which is what makes multi-MiB stacks per PSP thread cheap.
void *reserve_block(std::size_t size, void *) {
    const std::size_t rounded = round_up(size, page_size());
#if defined(_WIN32)
    return VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void *block = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, flags, -1, 0);
    return block == MAP_FAILED ? nullptr : block;
#endif
}

void release_block(void *block, std::size_t size, void *) {
    const std::size_t rounded = round_up(size, page_size());
#if defined(MPO_FIBER_ASAN)
    // Frames abandoned on this stack may leave poisoned shadow behind; the next
    // reservation can land on the same addresses.
    __asan_unpoison_memory_region(block, rounded);
#endif
#if defined(_WIN32)
    (void)rounded;
    VirtualFree(block, 0, MEM_RELEASE);
#else
    munmap(block, rounded);
#endif
}

bool protect_guard(void *page, std::size_t size, bool guard) {
#if defined(_WIN32)
    DWORD old = 0;
    return VirtualProtect(page, size, guard ? PAGE_NOACCESS : PAGE_READWRITE, &old) != 0;
#else
    return mprotect(page, size, guard ? PROT_NONE : PROT_READ | PROT_WRITE) == 0;
#endif
}

} // namespace

struct Fiber::Impl {
    mco_coro *coro{};
    Fiber *owner{};
    Entry entry;
    void *guard_page{};
    std::size_t guard_size{};
    std::size_t stack_size{};

    static void main(mco_coro *coro) {
        auto *self = static_cast<Impl *>(mco_get_user_data(coro));
        self->entry();
    }
};

std::unique_ptr<Fiber> Fiber::create(std::size_t stack_size, Entry entry) {
    if (stack_size < kMinStackSize) stack_size = kMinStackSize;
    const std::size_t page = page_size();
    // One extra page for the guard below the usable stack.
    stack_size = round_up(stack_size, page) + page;

    std::unique_ptr<Fiber> fiber(new Fiber());
    fiber->impl_ = std::make_unique<Impl>();
    Impl &impl = *fiber->impl_;
    impl.entry = std::move(entry);
    impl.owner = fiber.get();

    mco_desc desc = mco_desc_init(&Impl::main, stack_size);
    desc.user_data = &impl;
    desc.alloc_cb = &reserve_block;
    desc.dealloc_cb = &release_block;
    if (mco_create(&impl.coro, &desc) != MCO_SUCCESS) {
        impl.coro = nullptr;
        return nullptr;
    }
    // The stack grows down towards minicoro's header, so the first whole page
    // of the stack area becomes a guard: an overflow faults instead of
    // silently corrupting the coroutine context.
    const auto base = reinterpret_cast<std::uintptr_t>(impl.coro->stack_base);
    const std::uintptr_t guard = (base + page - 1u) / page * page;
    if (guard + page < base + impl.coro->stack_size &&
        protect_guard(reinterpret_cast<void *>(guard), page, true)) {
        impl.guard_page = reinterpret_cast<void *>(guard);
        impl.guard_size = page;
    }
    impl.stack_size = impl.coro->stack_size;
    return fiber;
}

Fiber::~Fiber() {
    if (!impl_ || impl_->coro == nullptr) return;
    if (impl_->guard_page != nullptr) protect_guard(impl_->guard_page, impl_->guard_size, false);
    // A suspended fiber is destroyed without unwinding: frames left on its
    // stack (an exited or terminated PSP thread) are simply discarded.
    if (mco_destroy(impl_->coro) != MCO_SUCCESS) {
        std::fprintf(stderr, "mpo_kernel: destroying a running fiber\n");
        std::abort();
    }
}

void Fiber::resume() {
    const mco_result result = mco_resume(impl_->coro);
    if (result != MCO_SUCCESS) {
        std::fprintf(stderr, "mpo_kernel: fiber resume failed: %s\n",
                     mco_result_description(result));
        std::abort();
    }
}

void Fiber::yield() {
    const mco_result result = mco_yield(impl_->coro);
    if (result != MCO_SUCCESS) {
        // MCO_STACK_OVERFLOW lands here when the magic number was smashed.
        std::fprintf(stderr, "mpo_kernel: fiber yield failed: %s\n",
                     mco_result_description(result));
        std::abort();
    }
}

bool Fiber::finished() const noexcept {
    return mco_status(impl_->coro) == MCO_DEAD;
}

bool Fiber::running() const noexcept {
    return mco_status(impl_->coro) == MCO_RUNNING;
}

std::size_t Fiber::stack_size() const noexcept {
    return impl_->stack_size;
}

Fiber *Fiber::current() noexcept {
    mco_coro *coro = mco_running();
    if (coro == nullptr) return nullptr;
    const auto *impl = static_cast<const Impl *>(mco_get_user_data(coro));
    return impl != nullptr ? impl->owner : nullptr;
}

} // namespace mpo::kernel
