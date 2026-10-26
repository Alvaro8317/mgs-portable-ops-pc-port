// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU state helpers, comparison, digests and the difftest driver.

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "difftest/difftest.hpp"

namespace mpo::difftest {

namespace {

bool in_window(std::uint32_t address, std::uint32_t size) {
    return address >= kMemBase && address - kMemBase <= kMemSize - size;
}

std::string hex32(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08" PRIx32, value);
    return text;
}

bool is_nan_bits(std::uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
}

// PPSSPP-style name of the VFPU register at voffset index i: S<matrix><column><row>.
std::string vfpu_reg_name(int index) {
    char text[16];
    std::snprintf(text, sizeof(text), "S%d%d%d", index / 16, (index / 4) % 4, index % 4);
    return text;
}

} // namespace

std::uint32_t CpuState::read32(std::uint32_t address) {
    if (!in_window(address, 4)) {
        ++mem_faults;
        return 0;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, &mem[address - kMemBase], 4);
    return value;
}

void CpuState::write32(std::uint32_t address, std::uint32_t value) {
    if (!in_window(address, 4)) {
        ++mem_faults;
        return;
    }
    std::memcpy(&mem[address - kMemBase], &value, 4);
}

std::uint32_t CpuState::read8(std::uint32_t address) {
    if (!in_window(address, 1)) {
        ++mem_faults;
        return 0;
    }
    return mem[address - kMemBase];
}

void CpuState::write8(std::uint32_t address, std::uint32_t value) {
    if (!in_window(address, 1)) {
        ++mem_faults;
        return;
    }
    mem[address - kMemBase] = static_cast<std::uint8_t>(value);
}

std::string_view class_name(InstrClass cls) {
    switch (cls) {
    case InstrClass::kAlu:
        return "alu";
    case InstrClass::kMulDiv:
        return "muldiv";
    case InstrClass::kMem:
        return "mem";
    case InstrClass::kAllegrex:
        return "allegrex";
    case InstrClass::kFpu:
        return "fpu";
    case InstrClass::kVfpu:
        return "vfpu";
    case InstrClass::kVfpuSeq:
        return "vfpuseq";
    }
    return "?";
}

std::span<const InstrClass> all_classes() {
    static constexpr InstrClass kAll[] = {
        InstrClass::kAlu, InstrClass::kMulDiv, InstrClass::kMem,     InstrClass::kAllegrex,
        InstrClass::kFpu, InstrClass::kVfpu,   InstrClass::kVfpuSeq,
    };
    return kAll;
}

bool parse_class(std::string_view text, InstrClass *out) {
    for (const InstrClass cls : all_classes()) {
        if (class_name(cls) == text) {
            *out = cls;
            return true;
        }
    }
    return false;
}

std::vector<Mismatch> compare(const CpuState &a, const CpuState &b, Policy policy) {
    std::vector<Mismatch> out;
    const auto check = [&out](const std::string &field, std::uint32_t x, std::uint32_t y) {
        if (x != y) out.push_back({field, x, y});
    };
    for (int i = 0; i < 32; ++i) check("gpr[" + std::to_string(i) + "]", a.gpr[i], b.gpr[i]);
    check("pc", a.pc, b.pc);
    check("hi", a.hi, b.hi);
    check("lo", a.lo, b.lo);
    const bool nan_equivalent = policy != Policy::kExact;
    for (int i = 0; i < 32; ++i) {
        if (nan_equivalent && is_nan_bits(a.fpr[i]) && is_nan_bits(b.fpr[i])) continue;
        check("fpr[" + std::to_string(i) + "]", a.fpr[i], b.fpr[i]);
    }
    if (policy == Policy::kMips32) {
        check("fcr31.c", (a.fcr31 >> 23) & 1, (b.fcr31 >> 23) & 1);
    } else {
        check("fcr31", a.fcr31, b.fcr31);
    }
    for (int i = 0; i < 128; ++i) {
        const std::uint32_t x = std::bit_cast<std::uint32_t>(a.v[i]);
        const std::uint32_t y = std::bit_cast<std::uint32_t>(b.v[i]);
        if (nan_equivalent && is_nan_bits(x) && is_nan_bits(y)) continue;
        check("v[" + vfpu_reg_name(i) + "]", x, y);
    }
    for (int i = 0; i < 16; ++i) {
        check("vfpu_ctrl[" + std::to_string(i) + "]", a.vfpu_ctrl[i], b.vfpu_ctrl[i]);
    }
    check("mem_faults", a.mem_faults, b.mem_faults);
    for (std::uint32_t offset = 0; offset < kMemSize; offset += 4) {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::memcpy(&x, &a.mem[offset], 4);
        std::memcpy(&y, &b.mem[offset], 4);
        check("mem[0x" + hex32(kMemBase + offset) + "]", x, y);
    }
    return out;
}

void digest_append(std::uint64_t *hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint64_t h = *hash;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 0x100000001B3ull;
    }
    *hash = h;
}

std::uint64_t digest(const CpuState &state, bool canonical_nan) {
    const auto canon = [canonical_nan](std::uint32_t bits) {
        return canonical_nan && is_nan_bits(bits) ? 0x7FC00000u : bits;
    };
    std::uint64_t hash = kDigestSeed;
    digest_append(&hash, state.gpr, sizeof(state.gpr));
    digest_append(&hash, &state.pc, sizeof(state.pc));
    digest_append(&hash, &state.hi, sizeof(state.hi));
    digest_append(&hash, &state.lo, sizeof(state.lo));
    for (const std::uint32_t reg : state.fpr) {
        const std::uint32_t bits = canon(reg);
        digest_append(&hash, &bits, sizeof(bits));
    }
    digest_append(&hash, &state.fcr31, sizeof(state.fcr31));
    for (const float value : state.v) {
        const std::uint32_t bits = canon(std::bit_cast<std::uint32_t>(value));
        digest_append(&hash, &bits, sizeof(bits));
    }
    digest_append(&hash, state.vfpu_ctrl, sizeof(state.vfpu_ctrl));
    digest_append(&hash, &state.mem_faults, sizeof(state.mem_faults));
    digest_append(&hash, state.mem.data(), state.mem.size());
    return hash;
}

Rng::Rng(std::uint64_t seed) {
    // splitmix64 to fill the xoshiro256** state.
    for (std::uint64_t &word : s_) {
        seed += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        word = z ^ (z >> 31);
    }
}

std::uint64_t Rng::next() {
    const std::uint64_t result = std::rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = std::rotl(s_[3], 45);
    return result;
}

std::uint32_t Rng::below(std::uint32_t bound) {
    // Multiply-shift: deterministic, tiny bias is irrelevant here.
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(next32()) * bound) >> 32);
}

std::uint32_t Rng::random_float_bits() {
    static constexpr std::uint32_t kSpecial[] = {
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x40000000,
        0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x7FBFFFFF,
        0x7FFFFFFF, 0x00000001, 0x807FFFFF, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF,
        0x4F000000, 0xCF000000, 0x4EFFFFFF, 0x3EFFFFFF, 0x3F7FFFFF, 0x3F800001, 0x40400000,
        0x3FC00000, 0xBFC00000, 0x40200000, 0x4B000000, 0x4B7FFFFF, 0x3E800000, 0x40800000,
    };
    switch (below(8)) {
    case 0:
        return kSpecial[below(sizeof(kSpecial) / sizeof(kSpecial[0]))];
    case 1:
        return next32(); // Any bit pattern, including NaNs and denormals.
    case 2: {
        // Small integers and halves: exercise rounding ties and exact results.
        const float value = static_cast<float>(static_cast<int>(below(65)) - 32) * 0.5f;
        return std::bit_cast<std::uint32_t>(value);
    }
    case 3: {
        // Angles in VFPU units ([-4, 4] is two full turns) and general [-8, 8].
        const float value = (static_cast<float>(next32() >> 8) / 16777216.0f) * 16.0f - 8.0f;
        return std::bit_cast<std::uint32_t>(value);
    }
    case 4: {
        // Values in [-1, 1] for asin/saturation.
        const float value = (static_cast<float>(next32() >> 8) / 16777216.0f) * 2.0f - 1.0f;
        return std::bit_cast<std::uint32_t>(value);
    }
    default: {
        // Random sign and mantissa, exponent around 1.0 (2^-20 .. 2^31).
        const std::uint32_t exponent = 107 + below(52);
        return (next32() & 0x807FFFFFu) | (exponent << 23);
    }
    }
}

std::uint32_t Rng::random_int_bits() {
    static constexpr std::uint32_t kSpecial[] = {
        0,          1,          2,          0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001,
        0xFFFF,     0x8000,     0x7FFF,     0xFFFF8000, 0xFF,       0x80,       0x7F,
        0xFFFFFF80, 0x0000FFFF, 0x00010000, 0x12345678, 0xDEADBEEF, 31,         32,
    };
    switch (below(4)) {
    case 0:
        return kSpecial[below(sizeof(kSpecial) / sizeof(kSpecial[0]))];
    case 1:
        return below(64) - 32u; // Small, possibly negative.
    default:
        return next32();
    }
}

bool run_difftest(Executor &a, Executor &b, std::span<const TestCase> cases, Policy policy,
                  int max_details, RunReport *report, std::string *error) {
    std::vector<CpuState> out_a(cases.size());
    std::vector<CpuState> out_b(cases.size());
    std::vector<std::uint8_t> ok_a(cases.size(), 1);
    std::vector<std::uint8_t> ok_b(cases.size(), 1);
    if (!a.run(cases, out_a, ok_a, error)) return false;
    if (!b.run(cases, out_b, ok_b, error)) return false;

    report->cases += static_cast<int>(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (!ok_a[i] || !ok_b[i]) {
            ++report->skipped;
            continue;
        }
        ++report->compared;
        const std::vector<Mismatch> diffs = compare(out_a[i], out_b[i], policy);
        if (diffs.empty()) {
            if (policy != Policy::kExact && !compare(out_a[i], out_b[i], Policy::kExact).empty()) {
                ++report->nan_only;
            }
            continue;
        }
        ++report->mismatched;
        if (static_cast<int>(report->details.size()) >= max_details) continue;
        std::string text = describe_case(cases[i]) + "\n";
        int shown = 0;
        for (const Mismatch &m : diffs) {
            if (shown++ == 8) {
                text += "    ...\n";
                break;
            }
            text += "    " + m.field + ": " + a.name() + "=" + hex32(m.a) + " " + b.name() + "=" +
                    hex32(m.b) + "\n";
        }
        report->details.push_back(std::move(text));
    }
    return true;
}

std::string describe_case(const TestCase &c) {
    std::string text = "case " + std::to_string(c.id) + " [" + std::string(class_name(c.cls)) +
                       "/" + c.spec + "] ops:";
    for (int i = 0; i < c.num_ops; ++i) text += " " + hex32(c.ops[static_cast<std::size_t>(i)]);
    text += " prefixes s=" + hex32(c.initial.vfpu_ctrl[0]) + " t=" + hex32(c.initial.vfpu_ctrl[1]) +
            " d=" + hex32(c.initial.vfpu_ctrl[2]);
    return text;
}

namespace {

class FaultInjector final : public Executor {
public:
    FaultInjector(std::unique_ptr<Executor> inner, std::string spec, int every)
        : inner_(std::move(inner)), spec_(std::move(spec)), every_(every) {}

    std::string name() const override { return "faulty-" + inner_->name(); }

    bool run(std::span<const TestCase> cases, std::span<CpuState> out,
             std::span<std::uint8_t> supported, std::string *error) override {
        if (!inner_->run(cases, out, supported, error)) return false;
        int seen = 0;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            if (!spec_.empty() && cases[i].spec != spec_) continue;
            if (seen++ % every_ != 0) continue;
            // Flip the lowest bit of a register the instruction class writes.
            switch (cases[i].cls) {
            case InstrClass::kFpu:
                out[i].fpr[(cases[i].ops[0] >> 6) & 31] ^= 1u;
                break;
            case InstrClass::kVfpu:
            case InstrClass::kVfpuSeq:
                out[i].v[i % 128] =
                    std::bit_cast<float>(std::bit_cast<std::uint32_t>(out[i].v[i % 128]) ^ 1u);
                break;
            default:
                out[i].hi ^= 1u;
                break;
            }
        }
        return true;
    }

private:
    std::unique_ptr<Executor> inner_;
    std::string spec_;
    int every_;
};

} // namespace

std::unique_ptr<Executor> make_fault_injector(std::unique_ptr<Executor> inner, std::string spec,
                                              int every) {
    return std::make_unique<FaultInjector>(std::move(inner), std::move(spec),
                                           every < 1 ? 1 : every);
}

} // namespace mpo::difftest
