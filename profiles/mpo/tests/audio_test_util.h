// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared helpers for the mpo_audio tests: a check macro, a fake PSP memory and
// a stable hash for golden vectors.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "guest_memory.h"

namespace audio_test {

inline int g_failures = 0;

inline void Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

template <typename A, typename B>
void ExpectEq(const A &actual, const B &expected, const std::string &message) {
    if (!(actual == expected)) {
        std::fprintf(stderr, "FAIL: %s (got %lld, want %lld)\n", message.c_str(),
                     static_cast<long long>(actual), static_cast<long long>(expected));
        ++g_failures;
    }
}

inline int Finish(const char *name) {
    if (g_failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}

// PSP user RAM stand-in: `size` bytes seen at 0x08800000.
class FakeRam {
public:
    static constexpr uint32_t kBase = 0x08800000;

    explicit FakeRam(uint32_t size = 4u << 20) : bytes_(size, 0) {}

    mpo::audio::GuestMemory View() {
        return mpo::audio::GuestMemory(bytes_.data(), kBase, static_cast<uint32_t>(bytes_.size()));
    }
    uint8_t *At(uint32_t addr) { return bytes_.data() + (addr - kBase); }
    void Put(uint32_t addr, const void *data, size_t size) { std::memcpy(At(addr), data, size); }
    void Put16(uint32_t addr, int16_t v) { std::memcpy(At(addr), &v, 2); }
    void Put32(uint32_t addr, uint32_t v) { std::memcpy(At(addr), &v, 4); }
    uint32_t Get32(uint32_t addr) {
        uint32_t v;
        std::memcpy(&v, At(addr), 4);
        return v;
    }
    int16_t Get16(uint32_t addr) {
        int16_t v;
        std::memcpy(&v, At(addr), 2);
        return v;
    }
    std::vector<uint8_t> &bytes() { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

// 64-bit FNV-1a.
inline uint64_t Fnv1a(const void *data, size_t size, uint64_t hash = 0xcbf29ce484222325ull) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Small deterministic PRNG (xorshift32), identical on every platform.
class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed ? seed : 1u) {}
    uint32_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    int Range(int lo, int hi) { // inclusive
        return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1));
    }

private:
    uint32_t state_;
};

} // namespace audio_test
