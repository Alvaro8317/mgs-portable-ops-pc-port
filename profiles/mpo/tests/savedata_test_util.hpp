// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared helpers for the mpo_savedata tests (F18a): checks, a flat buffer as
// PSP RAM, a virtual clock, request builders shaped like MPO's (from the
// contract in docs/analysis/utility.md; not game code) and dialog drivers.

#pragma once

#include "kernel/clock.hpp"
#include "kernel/guest_memory.hpp"
#include "savedata/dialog.hpp"
#include "savedata/psp_types.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace savedata_test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool ok, const char *expr, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++g_failures;
    }
}

template <class A, class B>
void check_eq(const A &a, const B &b, const char *ea, const char *eb, const char *file, int line) {
    ++g_checks;
    if (!(a == b)) {
        if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (0x%" PRIx64 " vs 0x%" PRIx64 ")\n", file,
                         line, ea, eb, static_cast<std::uint64_t>(a),
                         static_cast<std::uint64_t>(b));
        } else if constexpr (std::is_convertible_v<A, std::string> &&
                             std::is_convertible_v<B, std::string>) {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", file, line, ea, eb,
                         std::string(a).c_str(), std::string(b).c_str());
        } else {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", file, line, ea, eb);
        }
        ++g_failures;
    }
}

#define CHECK(x) ::savedata_test::check((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::savedata_test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

inline int finish(const char *name) {
    std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

using mpo::savedata::DialogStatus;

// 2 MiB of user RAM at 0x08800000.
struct Guest {
    static constexpr std::uint32_t kBase = 0x08800000u;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(2u << 20, 0);
    mpo::kernel::FlatGuestMemory memory{kBase, bytes};

    std::uint8_t *at(std::uint32_t address) { return memory.translate(address, 1); }
    template <class T>
    void put(std::uint32_t address, const T &value) {
        std::memcpy(memory.translate(address, sizeof(T)), &value, sizeof(T));
    }
    template <class T>
    T get(std::uint32_t address) {
        T value{};
        std::memcpy(&value, memory.translate(address, sizeof(T)), sizeof(T));
        return value;
    }
    void put_u16string(std::uint32_t address, const std::u16string &text) {
        for (std::size_t i = 0; i <= text.size(); ++i) {
            const char16_t c = i < text.size() ? text[i] : u'\0';
            std::uint8_t *p = memory.translate(address + static_cast<std::uint32_t>(2 * i), 2);
            p[0] = static_cast<std::uint8_t>(c & 0xFF);
            p[1] = static_cast<std::uint8_t>(c >> 8);
        }
    }
    std::u16string get_u16string(std::uint32_t address, std::size_t max = 512) {
        std::u16string out;
        for (std::size_t i = 0; i < max; ++i) {
            const std::uint8_t *p =
                memory.translate(address + static_cast<std::uint32_t>(2 * i), 2);
            const auto c = static_cast<char16_t>(p[0] | (p[1] << 8));
            if (c == 0) break;
            out.push_back(c);
        }
        return out;
    }
};

// The common header MPO's stages fill (FUN_08b97f5c, FUN_08bbd680,
// FUN_08bbd430 in title): language 1 (English), buttonSwap 0 when the
// system's confirm button is circle, thread priorities.
inline mpo::savedata::DialogCommon mpo_common(std::uint32_t size, std::int32_t button_swap,
                                              std::int32_t access_thread) {
    mpo::savedata::DialogCommon common{};
    common.size = size;
    common.language = 1;
    common.button_swap = button_swap;
    common.graphics_thread = 0x30;
    common.access_thread = access_thread;
    common.font_thread = 0x38;
    common.sound_thread = 0x28;
    return common;
}

// Presses: one Update with `button` newly pressed (and held).
inline mpo::savedata::ButtonState press(std::uint32_t button) {
    return {button, button};
}
inline mpo::savedata::ButtonState hold(std::uint32_t button) {
    return {button, 0};
}

// Advances the clock past the init delay and checks RUNNING.
template <class D>
void wait_running(D &dialog, mpo::kernel::VirtualClock &clock, std::int64_t delay_us) {
    clock.advance(static_cast<std::uint64_t>(delay_us));
    CHECK_EQ(static_cast<int>(dialog.status()), static_cast<int>(DialogStatus::kRunning));
}

// Calls Update(anim) with no input until the fade-in ends.
template <class D>
void finish_fade_in(D &dialog, Guest &guest, int anim = 2) {
    for (int i = 0; i < 64 && dialog.is_fading(); ++i) dialog.update(guest.memory, anim, {});
    CHECK(!dialog.is_fading());
}

// Calls Update(anim) with no input until FINISHED (or a bound).
template <class D>
int run_until_finished(D &dialog, Guest &guest, int anim = 2) {
    int updates = 0;
    while (dialog.status() == DialogStatus::kRunning && updates < 256) {
        dialog.update(guest.memory, anim, {});
        ++updates;
    }
    CHECK_EQ(static_cast<int>(dialog.status()), static_cast<int>(DialogStatus::kFinished));
    return updates;
}

} // namespace savedata_test
