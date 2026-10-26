// SPDX-License-Identifier: GPL-3.0-or-later
//
// A host fiber (minicoro coroutine) with an explicitly reserved stack. Each PSP
// thread runs on one of these. The stack is a lazily committed virtual memory
// reservation with a guard page at its low end, so a generous size costs
// address space, not RAM (see the stack trap in CLAUDE.md §3).

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace mpo::kernel {

class Fiber {
public:
    using Entry = std::function<void()>;

    // Smallest stack accepted, so the guard page never eats the whole stack.
    static constexpr std::size_t kMinStackSize = 64u * 1024u;

    // Returns nullptr when the stack cannot be reserved.
    static std::unique_ptr<Fiber> create(std::size_t stack_size, Entry entry);

    ~Fiber();
    Fiber(const Fiber &) = delete;
    Fiber &operator=(const Fiber &) = delete;

    // Switches from the caller (the scheduler) into the fiber until it yields
    // or returns. Must not be called from inside another fiber.
    void resume();
    // Switches from inside this fiber back to whoever resumed it.
    void yield();

    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::size_t stack_size() const noexcept;

    // The fiber executing on this host thread, or nullptr on the host stack.
    [[nodiscard]] static Fiber *current() noexcept;

private:
    Fiber() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mpo::kernel
