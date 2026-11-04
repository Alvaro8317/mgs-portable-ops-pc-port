// SPDX-License-Identifier: GPL-3.0-or-later
//
// F26a: the threads that read the replacement PNGs (and build their mip
// levels) away from the GE's thread, as PPSSPP's ReplacedTextureTask on its
// thread manager. Two queues: the textures a draw is waiting for go first,
// the pack's preload after them. Destroying the pool drops what is still
// queued and waits for the tasks running.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mpo::texreplace {

class DecodePool {
public:
    // `threads` <= 0: a default for this machine (half its cores, 1 to 4).
    explicit DecodePool(int threads = 0);
    ~DecodePool();
    DecodePool(const DecodePool &) = delete;
    DecodePool &operator=(const DecodePool &) = delete;

    // `urgent`: a draw waits for it (ahead of the preload).
    void Submit(std::function<void()> task, bool urgent);
    [[nodiscard]] int threads() const { return static_cast<int>(workers_.size()); }
    // Tasks queued and not started yet.
    [[nodiscard]] std::size_t queued() const;

private:
    void Work();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> urgent_;
    std::deque<std::function<void()>> background_;
    bool stop_ = false;
    std::vector<std::thread> workers_;
};

} // namespace mpo::texreplace
