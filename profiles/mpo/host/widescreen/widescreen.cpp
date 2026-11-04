// SPDX-License-Identifier: GPL-3.0-or-later

#include "widescreen.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "util/sha256.hpp"

namespace mpo::widescreen {
namespace {

// config/widescreen.toml, as a string literal (CMake writes it).
constexpr std::string_view kBuiltinToml =
#include "widescreen_profile.inc"
    ;

constexpr std::uint64_t kMaxTerm = 100000u;
constexpr std::uint64_t kPspW = kPspWidth;
constexpr std::uint64_t kPspH = kPspHeight;
constexpr std::uint64_t kMaxNum = kMaxAspectNumerator;

bool fail(std::string *error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

// Digits only, at most `max_digits` of them.
bool parse_digits(std::string_view text, std::size_t max_digits, std::uint64_t *out) {
    if (text.empty() || text.size() > max_digits) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10u + static_cast<std::uint64_t>(c - '0');
    }
    *out = value;
    return true;
}

bool is_sha256_hex(std::string_view value) {
    return value.size() == 64u && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, 4u); // the host is little-endian, as the PSP
    return value;
}

std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, 2u);
    return value;
}

} // namespace

double Aspect::ratio() const {
    if (native()) return static_cast<double>(kPspWidth) / static_cast<double>(kPspHeight);
    return static_cast<double>(width) / static_cast<double>(height);
}

std::string Aspect::text() const {
    if (native()) return "native";
    return std::to_string(width) + ":" + std::to_string(height);
}

std::optional<Aspect> ParseAspect(std::string_view text, std::string *error) {
    if (text == "native") return Aspect{};
    std::uint64_t w = 0;
    std::uint64_t h = 0;
    const std::size_t sep = text.find_first_of(":x/");
    const std::size_t dot = text.find('.');
    bool ok = false;
    if (sep != std::string_view::npos) {
        ok = parse_digits(text.substr(0, sep), 6u, &w) &&
             parse_digits(text.substr(sep + 1u), 6u, &h);
    } else if (dot != std::string_view::npos) {
        // A decimal, kept exact: 2.37 is 237:100.
        std::uint64_t whole = 0;
        std::uint64_t fraction = 0;
        const std::string_view digits = text.substr(dot + 1u);
        ok = parse_digits(text.substr(0, dot), 2u, &whole) && parse_digits(digits, 5u, &fraction);
        if (ok) {
            h = 1u;
            for (std::size_t i = 0; i < digits.size(); ++i) h *= 10u;
            w = whole * h + fraction;
        }
    } else {
        ok = parse_digits(text, 2u, &w);
        h = 1u;
    }
    if (!ok || w == 0u || h == 0u) {
        fail(error, "--aspect takes native, <width>:<height> (21:9, 2560:1080) or a decimal "
                    "(2.37), not '" +
                        std::string(text) + "'");
        return std::nullopt;
    }
    const std::uint64_t g = std::gcd(w, h);
    w /= g;
    h /= g;
    if (w > kMaxTerm || h > kMaxTerm) {
        fail(error, "--aspect " + std::string(text) + ": the numbers are too large");
        return std::nullopt;
    }
    // Narrower than the PSP's screen: the game cannot show less at the sides.
    if (w * kPspH < h * kPspW) {
        fail(error, "--aspect " + std::string(text) +
                        " is narrower than the PSP's screen (480x272): use native");
        return std::nullopt;
    }
    if (w > h * kMaxNum) {
        fail(error, "--aspect " + std::string(text) + " is wider than 4:1");
        return std::nullopt;
    }
    // Up to 16:9 the game is already as wide (0.7% from 480x272).
    if (w * 9u <= h * 16u) return Aspect{};
    return Aspect{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

std::uint32_t XScaleBits(const Aspect &aspect) {
    if (aspect.native()) return 0x3F800000u;
    // Both products are exact in a double, so the quotient is correctly
    // rounded, as Python's float(Fraction) in tools/widescreen.py; the float
    // rounds that to the nearest, as struct.pack('<f').
    const double value = (static_cast<double>(kPspWidth) * static_cast<double>(aspect.height)) /
                         (static_cast<double>(kPspHeight) * static_cast<double>(aspect.width));
    const auto f = static_cast<float>(value);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

float BitsToFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

std::optional<Profile> Profile::Parse(std::string_view toml_text, std::string *error) {
    toml::parse_result result = toml::parse(toml_text);
    if (!result) {
        fail(error, "widescreen.toml: " + std::string(result.error().description()));
        return std::nullopt;
    }
    const toml::table &root = result.table();
    const toml::array *modules = root["module"].as_array();
    if (modules == nullptr || modules->empty()) {
        fail(error, "widescreen.toml: no [[module]] entries");
        return std::nullopt;
    }
    Profile profile;
    for (const toml::node &node : *modules) {
        const toml::table *table = node.as_table();
        if (table == nullptr) {
            fail(error, "widescreen.toml: [[module]] must be a table");
            return std::nullopt;
        }
        ModuleProfile m;
        const auto name = (*table)["name"].value<std::string>();
        const auto sha = (*table)["sha256"].value<std::string>();
        const auto base = (*table)["image_base"].value<std::int64_t>();
        const auto address = (*table)["x_scale_address"].value<std::int64_t>();
        const auto original = (*table)["original"].value<std::int64_t>();
        if (!name || !sha || !base || !address || !original) {
            fail(error, "widescreen.toml: every [[module]] needs name, sha256, image_base, "
                        "x_scale_address and original");
            return std::nullopt;
        }
        const auto in_u32 = [](std::int64_t v) { return v >= 0 && v <= 0xFFFFFFFFll; };
        if (!is_sha256_hex(*sha) || !in_u32(*base) || !in_u32(*address) || !in_u32(*original) ||
            *address < *base || (*address & 3) != 0) {
            fail(error, "widescreen.toml: " + *name +
                            ": sha256 must be 64 lower-case hex digits and the address a "
                            "word inside the module");
            return std::nullopt;
        }
        m.name = *name;
        m.disc_id = (*table)["disc_id"].value_or(std::string());
        m.disc_version = (*table)["disc_version"].value_or(std::string());
        m.sha256 = *sha;
        m.image_base = static_cast<std::uint32_t>(*base);
        m.x_scale_address = static_cast<std::uint32_t>(*address);
        m.original = static_cast<std::uint32_t>(*original);
        profile.modules.push_back(std::move(m));
    }
    return profile;
}

std::string_view Profile::BuiltinText() {
    return kBuiltinToml;
}

const Profile &Profile::Builtin() {
    static const Profile profile = [] {
        std::string error;
        std::optional<Profile> parsed = Parse(kBuiltinToml, &error);
        // A broken file is a build problem; the tests parse it. An empty
        // profile refuses every module.
        if (!parsed) std::fprintf(stderr, "mpo_widescreen: built-in %s\n", error.c_str());
        return parsed ? std::move(*parsed) : Profile{};
    }();
    return profile;
}

std::optional<PatchResult> PatchModule(std::vector<std::uint8_t> &elf, const Profile &profile,
                                       std::uint32_t bits, std::string *error) {
    const std::string digest = mpo::sha256_hex(elf);
    const auto found = std::find_if(profile.modules.begin(), profile.modules.end(),
                                    [&](const ModuleProfile &m) { return m.sha256 == digest; });
    if (found == profile.modules.end()) {
        fail(error, "--aspect: this " + std::string(profile.modules.empty() ? "build" : "module") +
                        " is not a version widescreen.toml knows (SHA-256 " + digest + ")");
        return std::nullopt;
    }
    const ModuleProfile &m = *found;
    const std::span<const std::uint8_t> data(elf);
    constexpr std::uint8_t kIdent[] = {0x7F, 'E', 'L', 'F', 1, 1}; // 32-bit, little-endian
    if (data.size() < 52u || std::memcmp(data.data(), kIdent, sizeof kIdent) != 0) {
        fail(error, "--aspect: " + m.name + " is not a 32-bit little-endian ELF");
        return std::nullopt;
    }
    if (read_u16(data, 16u) != 0xFFA0u || read_u16(data, 18u) != 8u) {
        fail(error, "--aspect: " + m.name + " is not a relocatable PSP module (PRX)");
        return std::nullopt;
    }
    const std::uint32_t phoff = read_u32(data, 28u);
    const std::uint16_t phentsize = read_u16(data, 42u);
    const std::uint16_t phnum = read_u16(data, 44u);
    if (phnum == 0u || phentsize < 32u || phoff > data.size() || data.size() - phoff < 32u) {
        fail(error, "--aspect: " + m.name + " has no program headers");
        return std::nullopt;
    }
    const std::uint32_t p_type = read_u32(data, phoff);
    const std::uint32_t p_offset = read_u32(data, phoff + 4u);
    const std::uint32_t p_vaddr = read_u32(data, phoff + 8u);
    const std::uint32_t p_filesz = read_u32(data, phoff + 16u);
    if (p_type != 1u || p_vaddr != 0u || p_offset > data.size() ||
        data.size() - p_offset < p_filesz) {
        fail(error, "--aspect: the first program header of " + m.name + " is not its code segment");
        return std::nullopt;
    }
    const std::uint32_t relative = m.x_scale_address - m.image_base;
    if (relative > p_filesz || p_filesz - relative < 4u) {
        fail(error, "--aspect: the x scale address is outside the code segment of " + m.name);
        return std::nullopt;
    }
    const std::size_t offset = static_cast<std::size_t>(p_offset) + relative;
    const std::uint32_t current = read_u32(data, offset);
    if (current != m.original) {
        char text[96];
        std::snprintf(text, sizeof text, "0x%08X holds 0x%08X, not 0x%08X", m.x_scale_address,
                      current, m.original);
        fail(error, "--aspect: " + m.name + ": " + text + " (an already patched module?)");
        return std::nullopt;
    }
    std::memcpy(elf.data() + offset, &bits, sizeof bits);
    return PatchResult{&m, offset};
}

bool VideoFrameRule::OnFlip(std::uint64_t video_frames) {
    if (video_frames != last_frames_) {
        last_frames_ = video_frames;
        flips_since_ = 0;
    } else if (flips_since_ < ~std::uint64_t{0} / 2u) {
        ++flips_since_;
    }
    return flips_since_ < hold_;
}

} // namespace mpo::widescreen
