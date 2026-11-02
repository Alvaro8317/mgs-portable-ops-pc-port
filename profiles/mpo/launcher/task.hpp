// SPDX-License-Identifier: GPL-3.0-or-later
//
// One child process of the launcher (setup.py install, stages, play...),
// with its output read without blocking the UI (SDL_Process, F22b).

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "launcher_core.hpp"

struct SDL_Process;

namespace mpo::launcher {

class Task {
public:
    Task() = default;
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    ~Task();

    // False (with `error`) when the process could not start.
    bool start(const std::vector<std::string> &argv, std::string &error);
    // Reads what the process printed; returns the new complete lines.
    std::vector<std::string> poll();
    [[nodiscard]] bool running() const noexcept { return process_ != nullptr && !exit_code_; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] std::optional<int> exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] const LineBuffer &output() const noexcept { return output_; }
    [[nodiscard]] double seconds() const;
    // Forgets a finished process (its output too).
    void reset();

private:
    SDL_Process *process_{};
    bool started_{};
    std::optional<int> exit_code_;
    LineBuffer output_{2000};
    unsigned long long started_ticks_{};
    unsigned long long ended_ticks_{};
};

} // namespace mpo::launcher
