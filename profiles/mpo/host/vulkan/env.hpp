// SPDX-License-Identifier: GPL-3.0-or-later
//
// Environment variables without MSVC's C4996 on std::getenv.

#pragma once

#include <cstdlib>
#include <string>

namespace mpo::vk {

// The variable's value, or an empty string when it is not set.
inline std::string GetEnv(const char *name) {
#ifdef _MSC_VER
    char *value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

} // namespace mpo::vk
