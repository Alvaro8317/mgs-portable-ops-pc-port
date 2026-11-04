// SPDX-License-Identifier: GPL-3.0-or-later

#include "decode_pool.hpp"

#include <algorithm>
#include <utility>

namespace mpo::texreplace {

DecodePool::DecodePool(int threads) {
    if (threads <= 0) {
        const unsigned cores = std::thread::hardware_concurrency();
        threads = std::clamp(static_cast<int>(cores / 2), 1, 4);
    }
    workers_.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { Work(); });
}

DecodePool::~DecodePool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        urgent_.clear();
        background_.clear();
    }
    cv_.notify_all();
    for (std::thread &t : workers_) t.join();
}

void DecodePool::Submit(std::function<void()> task, bool urgent) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        (urgent ? urgent_ : background_).push_back(std::move(task));
    }
    cv_.notify_one();
}

std::size_t DecodePool::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return urgent_.size() + background_.size();
}

void DecodePool::Work() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !urgent_.empty() || !background_.empty(); });
            if (stop_) return;
            std::deque<std::function<void()>> &q = !urgent_.empty() ? urgent_ : background_;
            task = std::move(q.front());
            q.pop_front();
        }
        task();
    }
}

} // namespace mpo::texreplace
