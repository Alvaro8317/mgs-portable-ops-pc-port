// SPDX-License-Identifier: GPL-3.0-or-later

#include "apscan/virtual_ap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace mpo::apscan {
namespace {

// SplitMix64's finaliser: a fixed, portable 64-bit mixer.
constexpr std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Which property a hash is for, so the properties are independent.
enum Salt : std::uint64_t {
    kSaltBssid = 1,
    kSaltSsid = 2,
    kSaltChannel = 3,
    kSaltStrength = 4,
    kSaltSecurity = 5,
    kSaltJitter = 6,
};

std::uint64_t hash(std::uint64_t seed, std::int64_t epoch, std::uint64_t slot, std::uint64_t salt,
                   std::uint64_t extra = 0) {
    std::uint64_t h = mix(seed);
    h = mix(h ^ static_cast<std::uint64_t>(epoch));
    h = mix(h ^ slot);
    h = mix(h ^ salt);
    return mix(h ^ extra);
}

constexpr std::array<std::string_view, 16> kWords = {
    "Home", "Office", "Guest", "Cafe",  "Library", "Station", "Harbor", "Tower",
    "Lab",  "Studio", "Delta", "Alpha", "Orbit",   "Cedar",   "Falcon", "Summit",
};

constexpr std::string_view kHex = "0123456789ABCDEF";

std::string upper(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// Plausible home/office SSIDs, 4 to 15 printable ASCII characters.
std::string make_ssid(std::uint64_t h) {
    const std::string_view word = kWords[h & 15];
    const std::string_view word2 = kWords[(h >> 4) & 15];
    std::uint64_t digits = h >> 16;
    std::string ssid;
    switch ((h >> 8) & 3) {
    case 0: // HARBOR-3F2A
        ssid = upper(word) + '-';
        for (int i = 0; i < 4; ++i, digits >>= 4) ssid += kHex[digits & 15];
        break;
    case 1: // Cafe_042
        ssid = std::string(word) + '_';
        for (int i = 0; i < 3; ++i, digits /= 10) ssid += static_cast<char>('0' + digits % 10);
        break;
    case 2: // DELTA_LINK
        ssid = upper(word) + '_' + upper(word2);
        break;
    default: // HomeOrbit07
        ssid = std::string(word) + std::string(word2);
        for (int i = 0; i < 2; ++i, digits /= 10) ssid += static_cast<char>('0' + digits % 10);
        break;
    }
    return ssid;
}

std::array<std::uint8_t, kBssidSize> make_bssid(std::uint64_t h) {
    std::array<std::uint8_t, kBssidSize> bssid{};
    for (std::size_t i = 0; i < bssid.size(); ++i) {
        bssid[i] = static_cast<std::uint8_t>(h >> (8 * i));
    }
    // Unicast, locally administered: it can never be a real vendor's address.
    bssid[0] = static_cast<std::uint8_t>((bssid[0] & 0xFC) | 0x02);
    return bssid;
}

std::uint8_t make_channel(std::uint64_t h) {
    // Most 2.4 GHz networks sit on the non-overlapping channels 1, 6 and 11.
    constexpr std::array<std::uint8_t, 3> kCommon = {1, 6, 11};
    if ((h & 15) < 12) return kCommon[(h >> 4) % kCommon.size()];
    return static_cast<std::uint8_t>(1 + (h >> 8) % 13);
}

SecurityType make_security(std::uint64_t h) {
    switch (h % 10) {
    case 0:
    case 1:
        return SecurityType::kNone;
    case 2:
        return SecurityType::kWep;
    case 3:
    case 4:
        return SecurityType::kWpaTkip;
    default:
        return SecurityType::kWpaAes;
    }
}

} // namespace

std::string validate(const VirtualApConfig &config) {
    if (config.count == 0 || config.count > kMaxVirtualAps) {
        return "count must be between 1 and " + std::to_string(kMaxVirtualAps);
    }
    if (config.max_strength > 100) return "max_strength must be at most 100";
    if (config.min_strength > config.max_strength) {
        return "min_strength must not be above max_strength";
    }
    if (config.jitter > kMaxJitter) {
        return "jitter must be at most " + std::to_string(kMaxJitter);
    }
    return {};
}

VirtualApGenerator::VirtualApGenerator(const VirtualApConfig &config) : config_(config) {}

std::int64_t VirtualApGenerator::epoch_at(std::uint64_t now_us) const {
    if (config_.rotation_period_us == 0) return config_.epoch_offset;
    // PSP time stays far below 2^63 us, so the quotient fits.
    return static_cast<std::int64_t>(now_us / config_.rotation_period_us) + config_.epoch_offset;
}

std::vector<AccessPoint> VirtualApGenerator::access_points(std::int64_t epoch) const {
    const std::uint64_t seed = config_.seed;
    std::vector<AccessPoint> aps;
    aps.reserve(config_.count);
    for (std::uint64_t slot = 0; slot < config_.count; ++slot) {
        AccessPoint ap;
        // Retry with another hash on a clash, so BSSIDs and SSIDs stay unique.
        for (std::uint64_t attempt = 0;; ++attempt) {
            ap.bssid = make_bssid(hash(seed, epoch, slot, kSaltBssid, attempt));
            if (std::none_of(aps.begin(), aps.end(),
                             [&](const AccessPoint &other) { return other.bssid == ap.bssid; })) {
                break;
            }
        }
        for (std::uint64_t attempt = 0;; ++attempt) {
            ap.ssid = make_ssid(hash(seed, epoch, slot, kSaltSsid, attempt));
            if (std::none_of(aps.begin(), aps.end(),
                             [&](const AccessPoint &other) { return other.ssid == ap.ssid; })) {
                break;
            }
        }
        ap.channel = make_channel(hash(seed, epoch, slot, kSaltChannel));
        const std::uint64_t span = config_.max_strength - config_.min_strength + 1u;
        ap.strength = static_cast<std::uint8_t>(config_.min_strength +
                                                hash(seed, epoch, slot, kSaltStrength) % span);
        ap.security = make_security(hash(seed, epoch, slot, kSaltSecurity));
        aps.push_back(std::move(ap));
    }
    return aps;
}

std::vector<AccessPoint> VirtualApGenerator::scan(std::uint64_t now_us,
                                                  std::uint64_t scan_index) const {
    const std::int64_t epoch = epoch_at(now_us);
    std::vector<AccessPoint> aps = access_points(epoch);
    if (config_.jitter == 0) return aps;
    const int jitter = config_.jitter;
    for (std::uint64_t slot = 0; slot < aps.size(); ++slot) {
        const std::uint64_t h = hash(config_.seed, epoch, slot, kSaltJitter, scan_index);
        const int delta = static_cast<int>(h % static_cast<std::uint64_t>(2 * jitter + 1)) - jitter;
        const int strength = std::clamp(aps[slot].strength + delta, 0, 100);
        aps[slot].strength = static_cast<std::uint8_t>(strength);
    }
    return aps;
}

} // namespace mpo::apscan
