// SPDX-License-Identifier: GPL-3.0-or-later
//
// One child process of the launcher (F22b); see task.hpp.

#include "task.hpp"

#include <SDL3/SDL.h>

namespace mpo::launcher {

Task::~Task() {
    // A step that is still running goes on by itself (setup.py resumes and
    // holds its own lock); the launcher only stops reading it.
    if (process_ != nullptr) SDL_DestroyProcess(process_);
}

bool Task::start(const std::vector<std::string> &argv, std::string &error) {
    reset();
    std::vector<const char *> args;
    args.reserve(argv.size() + 1u);
    for (const std::string &arg : argv) args.push_back(arg.c_str());
    args.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    process_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (process_ == nullptr) {
        error = SDL_GetError();
        return false;
    }
    started_ = true;
    started_ticks_ = SDL_GetTicks();
    return true;
}

std::vector<std::string> Task::poll() {
    std::vector<std::string> lines;
    if (process_ == nullptr || exit_code_) return lines;
    SDL_IOStream *out = static_cast<SDL_IOStream *>(SDL_GetPointerProperty(
        SDL_GetProcessProperties(process_), SDL_PROP_PROCESS_STDOUT_POINTER, nullptr));
    char buffer[4096];
    bool eof = out == nullptr;
    while (out != nullptr) {
        const std::size_t got = SDL_ReadIO(out, buffer, sizeof buffer);
        if (got > 0u) {
            std::vector<std::string> more = output_.append(std::string_view(buffer, got));
            lines.insert(lines.end(), more.begin(), more.end());
            continue;
        }
        eof = SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY;
        break;
    }
    int code = 0;
    // Only once the output is drained: the last lines say why it stopped.
    if (eof && SDL_WaitProcess(process_, false, &code)) {
        std::vector<std::string> rest = output_.finish();
        lines.insert(lines.end(), rest.begin(), rest.end());
        exit_code_ = code;
        ended_ticks_ = SDL_GetTicks();
        SDL_DestroyProcess(process_);
        process_ = nullptr;
    }
    return lines;
}

double Task::seconds() const {
    if (!started_) return 0.0;
    const unsigned long long end = exit_code_ ? ended_ticks_ : SDL_GetTicks();
    return static_cast<double>(end - started_ticks_) / 1000.0;
}

void Task::reset() {
    if (process_ != nullptr) SDL_DestroyProcess(process_);
    process_ = nullptr;
    started_ = false;
    exit_code_.reset();
    output_.clear();
}

} // namespace mpo::launcher
