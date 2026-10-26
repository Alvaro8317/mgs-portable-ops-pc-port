// SPDX-License-Identifier: GPL-3.0-or-later
//
// mpo_apscan (F20a): seeded virtual APs and the sceNetApctl scan model, on a
// virtual clock and a flat buffer for PSP RAM. "The stage" below is a
// behavioural stand-in for MPO's apscan/briefing stage, written from the
// contract in docs/analysis/apscan.md (handler conditions, ID list walk, the
// fields it reads); it is not game code. No game data is needed.

#include "apscan/apctl_model.hpp"
#include "apscan/scan_source.hpp"
#include "apscan/virtual_ap.hpp"
#include "kernel/clock.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace ap = mpo::apscan;
namespace err = mpo::apscan::error;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *expr, const char *file, int line) {
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
        } else {
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", file, line, ea, eb);
        }
        ++g_failures;
    }
}

#define CHECK(x) check((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a, #b, __FILE__, __LINE__)

constexpr std::uint32_t kState(ap::ApctlState s) {
    return static_cast<std::uint32_t>(s);
}
constexpr std::uint32_t kEvent(ap::ApctlEvent e) {
    return static_cast<std::uint32_t>(e);
}

constexpr std::uint32_t kDisconnected = kState(ap::ApctlState::kDisconnected);
constexpr std::uint32_t kScanning = kState(ap::ApctlState::kScanning);
constexpr std::uint32_t kScanRequest = kEvent(ap::ApctlEvent::kScanRequest);
constexpr std::uint32_t kScanComplete = kEvent(ap::ApctlEvent::kScanComplete);
constexpr std::uint32_t kErrorEvent = kEvent(ap::ApctlEvent::kError);
constexpr std::uint32_t kDisconnectRequest = kEvent(ap::ApctlEvent::kDisconnectRequest);

// The stage's handler and object, at addresses MPO uses (0x08BAECC4 is the
// apscan handler; the object address is arbitrary).
constexpr std::uint32_t kHandlerEntry = 0x08BAECC4u;
constexpr std::uint32_t kStageObject = 0x09A00000u;
constexpr std::uint64_t kScanUs = 700'000;

// PSP RAM for the guest buffers.
class Ram {
public:
    static constexpr std::uint32_t kBase = 0x09F00000u;
    Ram() : bytes_(0x1000, 0xCD) {}
    std::span<std::uint8_t> at(std::uint32_t address, std::uint32_t size) {
        return std::span<std::uint8_t>(bytes_).subspan(address - kBase, size);
    }
    std::uint32_t u32(std::uint32_t address) {
        auto b = at(address, 4);
        std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i) value = value << 8 | b[static_cast<std::size_t>(i)];
        return value;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

// What the stage keeps of one AP (the fields of its 0x48-byte record).
struct StageAp {
    std::array<std::uint8_t, 6> bssid{};
    std::string ssid;
    std::uint8_t strength = 0;
};

// Behavioural stand-in for the stage (docs/analysis/apscan.md).
struct Stage {
    std::uint32_t flags = 0; // +0x90: bit 0 scan complete, bit 1 error
    std::uint32_t error = 0; // +0x94

    void handle(const ap::HandlerCall &call) {
        CHECK_EQ(call.entry, kHandlerEntry);
        CHECK_EQ(call.arg, kStageObject);
        if (call.new_state != kDisconnected) return;
        if (call.event == kErrorEvent) {
            error = call.error;
            flags |= 2;
        } else if (call.event == kScanComplete) {
            flags |= 1;
        }
    }

    // sceNetApctlScanUser, then the flags are cleared after it returns.
    void start_scan(ap::ApctlModel &model) {
        CHECK_EQ(model.scan_user(), 0);
        flags = 0;
    }

    // The Apctl thread of the HLE: deliver whatever is pending.
    void drain(ap::ApctlModel &model) {
        model.update();
        while (auto call = model.pop_handler_call()) handle(*call);
    }

    // Reads the results like the stage: a 0x80-byte ID list, then infoIds 4,
    // 0, 1 and 2 of each entry, skipping APs at 9 or less.
    std::vector<StageAp> read_results(ap::ApctlModel &model, Ram &ram) {
        constexpr std::uint32_t kList = Ram::kBase;
        constexpr std::uint32_t kEntry = Ram::kBase + 0x100;
        const std::uint32_t size = model.write_bss_id_list(kList, ram.at(kList, 0x80));
        std::vector<StageAp> aps;
        if (size == 0) return aps;
        std::uint32_t node = kList;
        while (node != 0) {
            const auto id = static_cast<std::int32_t>(ram.u32(node + 4));
            StageAp entry;
            CHECK_EQ(model.get_bss_desc_entry(id, 4, ram.at(kEntry, 4)), 0);
            entry.strength = ram.at(kEntry, 1)[0];
            if (entry.strength > 9) {
                CHECK_EQ(model.get_bss_desc_entry(id, 0, ram.at(kEntry, 8)), 0);
                std::copy_n(ram.at(kEntry, 6).begin(), 6, entry.bssid.begin());
                CHECK_EQ(model.get_bss_desc_entry(id, 1, ram.at(kEntry, 32)), 0);
                const auto ssid = ram.at(kEntry, 32);
                std::string raw(ssid.begin(), ssid.end());
                CHECK_EQ(model.get_bss_desc_entry(id, 2, ram.at(kEntry, 4)), 0);
                const std::uint32_t length = std::min<std::uint32_t>(ram.u32(kEntry), 32);
                entry.ssid = raw.substr(0, length).c_str();
                aps.push_back(entry);
            }
            node = ram.u32(node);
        }
        return aps;
    }
};

std::string bssid_text(const std::array<std::uint8_t, 6> &bssid) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2],
                  bssid[3], bssid[4], bssid[5]);
    return text;
}

ap::VirtualApConfig config_with_seed(std::uint64_t seed) {
    ap::VirtualApConfig config;
    config.seed = seed;
    return config;
}

// --- Generator ----------------------------------------------------------------

// Pinned output: the generator must give these exact APs on every compiler
// and platform, or the recruits of a seed would change between builds.
void test_generator_golden() {
    const ap::VirtualApGenerator generator(config_with_seed(0x4D504F));
    const auto aps = generator.access_points(0);
    struct Golden {
        const char *bssid;
        const char *ssid;
        std::uint8_t channel;
        std::uint8_t strength;
        ap::SecurityType security;
    };
    using S = ap::SecurityType;
    const Golden golden[] = {
        {"32:B8:0B:79:D5:51", "Lab_973", 11, 50, S::kWpaAes},
        {"CE:7B:9A:F3:60:CA", "GUEST-CE48", 11, 40, S::kWpaTkip},
        {"22:57:3F:9C:11:45", "HomeHarbor92", 11, 68, S::kWpaAes},
        {"06:77:92:BF:92:A7", "Studio_400", 1, 34, S::kWpaAes},
        {"72:CE:CE:B9:22:8E", "CEDAR-36C4", 1, 83, S::kWpaAes},
        {"F2:3A:A1:D3:59:CB", "DELTA-3E2B", 6, 53, S::kWpaAes},
        {"7E:34:7A:CA:FB:6B", "OrbitCedar45", 6, 83, S::kWep},
        {"26:C4:CF:27:05:4E", "HomeCafe11", 1, 40, S::kWpaAes},
    };
    CHECK_EQ(aps.size(), std::size(golden));
    for (std::size_t i = 0; i < std::min(aps.size(), std::size(golden)); ++i) {
        CHECK_EQ(bssid_text(aps[i].bssid), std::string(golden[i].bssid));
        CHECK_EQ(aps[i].ssid, std::string(golden[i].ssid));
        CHECK_EQ(aps[i].channel, golden[i].channel);
        CHECK_EQ(aps[i].strength, golden[i].strength);
        CHECK(aps[i].security == golden[i].security);
    }
    // The per-scan jitter is pinned too (scan 3 of epoch 0).
    const auto jittered = generator.scan(0, 3);
    const std::uint8_t kJittered[] = {49, 41, 69, 35, 81, 56, 85, 43};
    CHECK_EQ(jittered.size(), std::size(kJittered));
    for (std::size_t i = 0; i < std::min(jittered.size(), std::size(kJittered)); ++i) {
        CHECK_EQ(jittered[i].strength, kJittered[i]);
    }
}

void test_generator_stable_and_distinct() {
    const ap::VirtualApGenerator a(config_with_seed(1));
    const ap::VirtualApGenerator b(config_with_seed(1));
    const ap::VirtualApGenerator c(config_with_seed(2));
    CHECK(a.access_points(0) == b.access_points(0));
    CHECK(a.access_points(5) == b.access_points(5));
    CHECK(a.scan(123, 7) == b.scan(123, 7));
    CHECK(a.access_points(0) != c.access_points(0));
    CHECK(a.access_points(0) != a.access_points(1));
    // A different seed or epoch shares no BSSID.
    std::set<std::array<std::uint8_t, 6>> seen;
    for (const auto &x : a.access_points(0)) seen.insert(x.bssid);
    for (const auto &x : c.access_points(0)) CHECK(!seen.contains(x.bssid));
    for (const auto &x : a.access_points(1)) CHECK(!seen.contains(x.bssid));
}

void test_generator_constraints() {
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        ap::VirtualApConfig config = config_with_seed(seed * 0x9E3779B97F4A7C15ull);
        config.count = ap::kMaxVirtualAps;
        config.min_strength = 10;
        config.max_strength = 100;
        const ap::VirtualApGenerator generator(config);
        for (std::int64_t epoch : {-3, 0, 1, 1000}) {
            const auto aps = generator.access_points(epoch);
            CHECK_EQ(aps.size(), std::size_t{ap::kMaxVirtualAps});
            std::set<std::array<std::uint8_t, 6>> bssids;
            std::set<std::string> ssids;
            for (const auto &x : aps) {
                CHECK_EQ(x.bssid[0] & 0x03, 0x02); // unicast, locally administered
                CHECK(!x.ssid.empty() && x.ssid.size() <= ap::kSsidSize);
                CHECK(std::all_of(x.ssid.begin(), x.ssid.end(),
                                  [](char ch) { return ch >= 0x20 && ch < 0x7F; }));
                CHECK(x.channel >= 1 && x.channel <= 13);
                CHECK(x.strength >= 10 && x.strength <= 100);
                CHECK(static_cast<std::uint32_t>(x.security) <= 4 &&
                      x.security != ap::SecurityType::kUnsupported);
                bssids.insert(x.bssid);
                ssids.insert(x.ssid);
            }
            CHECK_EQ(bssids.size(), aps.size());
            CHECK_EQ(ssids.size(), aps.size());
        }
    }
    // A single fixed strength.
    ap::VirtualApConfig fixed = config_with_seed(9);
    fixed.min_strength = fixed.max_strength = 55;
    fixed.jitter = 0;
    for (const auto &x : ap::VirtualApGenerator(fixed).scan(0, 3)) CHECK_EQ(x.strength, 55);
}

void test_generator_jitter() {
    ap::VirtualApConfig config = config_with_seed(77);
    config.jitter = 4;
    const ap::VirtualApGenerator generator(config);
    const auto base = generator.access_points(0);
    bool varied = false;
    for (std::uint64_t scan = 0; scan < 50; ++scan) {
        const auto aps = generator.scan(1000, scan);
        CHECK(aps == generator.scan(1000, scan));
        CHECK_EQ(aps.size(), base.size());
        for (std::size_t i = 0; i < aps.size(); ++i) {
            CHECK(aps[i].bssid == base[i].bssid);
            CHECK_EQ(aps[i].ssid, base[i].ssid);
            const int delta = aps[i].strength - base[i].strength;
            CHECK(delta >= -4 && delta <= 4);
            varied = varied || delta != 0;
        }
    }
    CHECK(varied);
    // Jitter never leaves 0..100.
    ap::VirtualApConfig edge = config_with_seed(5);
    edge.min_strength = 0;
    edge.max_strength = 2;
    edge.jitter = ap::kMaxJitter;
    for (std::uint64_t scan = 0; scan < 50; ++scan) {
        for (const auto &x : ap::VirtualApGenerator(edge).scan(0, scan)) CHECK(x.strength <= 22);
    }
}

void test_generator_rotation() {
    ap::VirtualApConfig config = config_with_seed(3);
    config.rotation_period_us = 1'000'000;
    ap::VirtualApGenerator generator(config);
    CHECK_EQ(generator.epoch_at(0), 0);
    CHECK_EQ(generator.epoch_at(999'999), 0);
    CHECK_EQ(generator.epoch_at(1'000'000), 1);
    CHECK_EQ(generator.epoch_at(5'500'000), 5);
    CHECK(generator.scan(10, 0)[0].bssid == generator.scan(999'999, 0)[0].bssid);
    CHECK(generator.scan(10, 0)[0].bssid != generator.scan(1'000'000, 0)[0].bssid);

    config.epoch_offset = 7;
    CHECK_EQ(ap::VirtualApGenerator(config).epoch_at(2'000'000), 9);
    config.rotation_period_us = 0;
    CHECK_EQ(ap::VirtualApGenerator(config).epoch_at(~0ull), 7);

    // Default: one AP set per 24 h of PSP time.
    const ap::VirtualApGenerator daily(config_with_seed(3));
    CHECK_EQ(daily.epoch_at(ap::kDefaultRotationPeriodUs - 1), 0);
    CHECK_EQ(daily.epoch_at(ap::kDefaultRotationPeriodUs), 1);
}

void test_validate() {
    CHECK(ap::validate(ap::VirtualApConfig{}).empty());
    ap::VirtualApConfig bad;
    bad.count = 0;
    CHECK(!ap::validate(bad).empty());
    bad = {};
    bad.count = ap::kMaxVirtualAps + 1;
    CHECK(!ap::validate(bad).empty());
    bad = {};
    bad.max_strength = 101;
    CHECK(!ap::validate(bad).empty());
    bad = {};
    bad.min_strength = 60;
    bad.max_strength = 50;
    CHECK(!ap::validate(bad).empty());
    bad = {};
    bad.jitter = ap::kMaxJitter + 1;
    CHECK(!ap::validate(bad).empty());
}

// --- Model ----------------------------------------------------------------------

struct Fixture {
    mpo::kernel::VirtualClock clock{1'000'000};
    ap::VirtualApSource source{config_with_seed(0x4D504F)};
    ap::ApctlModel model{clock, source};
    Stage stage;
    Ram ram;
    std::int32_t handler_id = -1;

    Fixture() {
        CHECK_EQ(model.init(), 0);
        handler_id = model.add_handler(kHandlerEntry, kStageObject);
        CHECK_EQ(handler_id, 0);
    }
};

ap::HandlerCall call(std::int32_t id, std::uint32_t old_state, std::uint32_t new_state,
                     std::uint32_t event, std::uint32_t error = 0) {
    return ap::HandlerCall{id, kHandlerEntry, old_state, new_state, event, error, kStageObject};
}

void test_scan_sequence() {
    Fixture f;
    CHECK(f.model.state() == ap::ApctlState::kDisconnected);
    CHECK(!f.model.next_deadline_us());
    CHECK_EQ(f.model.scan_user(), 0);
    // Nothing is delivered inside the call: the stage clears its flags after it.
    CHECK(f.model.state() == ap::ApctlState::kScanning);
    CHECK_EQ(f.model.pending_handler_calls(), std::size_t{1});
    CHECK(f.model.next_deadline_us() == std::optional<std::uint64_t>(1'000'000));
    CHECK(f.model.pop_handler_call() == call(0, kDisconnected, kScanning, kScanRequest));
    CHECK(f.model.next_deadline_us() == std::optional<std::uint64_t>(1'000'000 + kScanUs));

    f.clock.advance(kScanUs - 1);
    f.model.update();
    CHECK(f.model.state() == ap::ApctlState::kScanning);
    CHECK(!f.model.pop_handler_call());
    CHECK(f.model.results().empty());

    f.clock.advance(1);
    f.model.update();
    CHECK(f.model.state() == ap::ApctlState::kDisconnected);
    // SCAN_COMPLETE goes SCANNING -> DISCONNECTED, the shape the stage needs.
    CHECK(f.model.pop_handler_call() == call(0, kScanning, kDisconnected, kScanComplete));
    CHECK(!f.model.pop_handler_call());
    CHECK(!f.model.next_deadline_us());
    CHECK_EQ(f.model.results().size(), std::size_t{8});
    CHECK_EQ(f.model.scans_started(), 1u);
    CHECK_EQ(f.model.scans_completed(), 1u);
    // The results are the source's scan 0, taken at the deadline.
    CHECK(f.model.results() == f.source.generator().scan(1'000'000 + kScanUs, 0));

    // A late update() gives the same results.
    Fixture late;
    late.model.scan_user();
    late.clock.advance(kScanUs * 3);
    late.model.update();
    CHECK(late.model.results() == f.model.results());
}

// The stage's scanning state: it waits for bit 0, reads the results and scans
// again at once; the HLE's Apctl thread delivers the calls in between.
void test_stage_loop() {
    Fixture f;
    f.stage.start_scan(f.model);
    std::uint64_t frames = 0;
    int recruits_read = 0;
    while (recruits_read < 10 && frames < 10'000) {
        f.clock.advance(16'667); // one 60 Hz frame
        ++frames;
        f.stage.drain(f.model);
        CHECK_EQ(f.stage.flags & 2u, 0u);
        if (f.stage.flags & 1u) {
            const auto aps = f.stage.read_results(f.model, f.ram);
            CHECK_EQ(aps.size(), std::size_t{8});
            ++recruits_read;
            f.stage.start_scan(f.model);
        }
    }
    CHECK_EQ(recruits_read, 10);
    CHECK_EQ(f.model.scans_completed(), 10u);
    // 700 ms per scan at 60 Hz: 42 frames each.
    CHECK(frames >= 10 * 42 && frames <= 10 * 43);
    // What the stage read matches what the model holds.
    const auto aps = f.stage.read_results(f.model, f.ram);
    for (std::size_t i = 0; i < aps.size(); ++i) {
        CHECK(aps[i].bssid == f.model.results()[i].bssid);
        CHECK_EQ(aps[i].ssid, f.model.results()[i].ssid);
        CHECK_EQ(aps[i].strength, f.model.results()[i].strength);
    }
}

void test_id_list_layout() {
    Fixture f;
    f.model.scan_user();
    f.clock.advance(kScanUs);
    f.model.update();
    CHECK_EQ(f.model.results().size(), std::size_t{8});

    // Whole list in a 0x80 buffer.
    constexpr std::uint32_t kBuf = Ram::kBase + 0x40;
    CHECK_EQ(f.model.write_bss_id_list(kBuf, f.ram.at(kBuf, 0x80)), 64u);
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint32_t next = i < 7 ? kBuf + (i + 1) * 8 : 0;
        CHECK_EQ(f.ram.u32(kBuf + i * 8), next);
        CHECK_EQ(f.ram.u32(kBuf + i * 8 + 4), i);
    }
    CHECK_EQ(f.ram.u32(kBuf + 64), 0xCDCDCDCDu); // past the list: untouched

    // A 20-byte buffer takes two entries; the second ends the list.
    Ram small;
    CHECK_EQ(f.model.write_bss_id_list(kBuf, small.at(kBuf, 20)), 64u);
    CHECK_EQ(small.u32(kBuf), kBuf + 8);
    CHECK_EQ(small.u32(kBuf + 4), 0u);
    CHECK_EQ(small.u32(kBuf + 8), 0u);
    CHECK_EQ(small.u32(kBuf + 12), 1u);
    CHECK_EQ(small.u32(kBuf + 16), 0xCDCDCDCDu);

    // A null buffer only gets the size.
    CHECK_EQ(f.model.write_bss_id_list(0, {}), 64u);

    // No scan yet: empty list.
    Fixture empty;
    Ram untouched;
    CHECK_EQ(empty.model.write_bss_id_list(kBuf, untouched.at(kBuf, 0x80)), 0u);
    CHECK_EQ(untouched.u32(kBuf), 0xCDCDCDCDu);
}

void test_entry_layout() {
    mpo::kernel::VirtualClock clock;
    struct Fixed final : ap::ScanSource {
        ap::ScanOutcome scan(const ap::ScanRequest &) override {
            ap::AccessPoint a;
            a.bssid = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
            a.ssid = "Harbor_042";
            a.channel = 11;
            a.strength = 73;
            a.security = ap::SecurityType::kWpaAes;
            ap::AccessPoint b = a;
            b.ssid = std::string(40, 'x'); // longer than an SSID can be
            return {0, {a, b}};
        }
    } source;
    ap::ApctlModel model(clock, source);
    model.init();
    model.scan_user();
    clock.advance(kScanUs);
    model.update();
    Ram ram;
    constexpr std::uint32_t kOut = Ram::kBase;

    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(0), 6u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(1), 32u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(2), 4u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(3), 1u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(4), 1u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(5), 4u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(6), 0u);
    CHECK_EQ(ap::ApctlModel::bss_desc_entry_size(-1), 0u);

    CHECK_EQ(model.get_bss_desc_entry(0, 0, ram.at(kOut, 8)), 0);
    const std::uint8_t bssid[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0xCD, 0xCD};
    CHECK(std::equal(std::begin(bssid), std::end(bssid), ram.at(kOut, 8).begin()));

    CHECK_EQ(model.get_bss_desc_entry(0, 1, ram.at(kOut, 33)), 0);
    const auto ssid = ram.at(kOut, 33);
    CHECK_EQ(std::string(reinterpret_cast<const char *>(ssid.data())), std::string("Harbor_042"));
    CHECK(std::all_of(ssid.begin() + 10, ssid.begin() + 32, [](std::uint8_t c) { return c == 0; }));
    CHECK_EQ(ssid[32], 0xCD);

    CHECK_EQ(model.get_bss_desc_entry(0, 2, ram.at(kOut, 4)), 0);
    CHECK_EQ(ram.u32(kOut), 10u);
    CHECK_EQ(model.get_bss_desc_entry(1, 2, ram.at(kOut, 4)), 0);
    CHECK_EQ(ram.u32(kOut), 32u); // clamped to the SSID size
    CHECK_EQ(model.get_bss_desc_entry(1, 1, ram.at(kOut, 32)), 0);
    CHECK_EQ(ram.at(kOut, 32)[31], 'x');

    Ram fresh;
    CHECK_EQ(model.get_bss_desc_entry(0, 3, fresh.at(kOut, 4)), 0);
    CHECK_EQ(fresh.u32(kOut), 0xCDCDCD0Bu); // one byte: channel 11
    CHECK_EQ(model.get_bss_desc_entry(0, 4, fresh.at(kOut, 4)), 0);
    CHECK_EQ(fresh.u32(kOut), 0xCDCDCD49u); // one byte: 73 %
    CHECK_EQ(model.get_bss_desc_entry(0, 5, fresh.at(kOut, 4)), 0);
    CHECK_EQ(fresh.u32(kOut), 4u);

    // Errors.
    CHECK_EQ(model.get_bss_desc_entry(0, 6, fresh.at(kOut, 4)), ap::result(err::kInvalidCode));
    CHECK_EQ(model.get_bss_desc_entry(2, 0, fresh.at(kOut, 8)), ap::result(err::kInvalidId));
    CHECK_EQ(model.get_bss_desc_entry(-1, 0, fresh.at(kOut, 8)), ap::result(err::kInvalidId));
    CHECK_EQ(model.get_bss_desc_entry(0, 1, fresh.at(kOut, 31)), ap::result(err::kGeneric));
}

void test_errors() {
    Fixture f;
    CHECK_EQ(f.model.init(), ap::result(err::kAlreadyInitialized));
    CHECK_EQ(f.model.scan_user(), 0);
    CHECK_EQ(f.model.scan_user(), ap::result(err::kNotDisconnected));
    CHECK_EQ(f.model.scans_started(), 1u);
    CHECK_EQ(f.model.del_handler(5), ap::result(err::kGeneric));

    // WLAN switch off.
    Fixture off;
    off.model.set_wlan_switch(false);
    CHECK(!off.model.wlan_switch());
    CHECK_EQ(off.model.scan_user(), ap::result(err::kWlanSwitchOff));
    CHECK(off.model.state() == ap::ApctlState::kDisconnected);
    CHECK_EQ(off.model.pending_handler_calls(), std::size_t{0});

    // A scan that fails mid-way: ERROR with newState DISCONNECTED, which the
    // stage turns into its error dialog.
    Fixture fail;
    fail.stage.start_scan(fail.model);
    fail.stage.drain(fail.model);
    fail.model.fail_scan(err::kWlanSwitchOff);
    fail.stage.drain(fail.model);
    CHECK_EQ(fail.stage.flags, 2u);
    CHECK_EQ(fail.stage.error, err::kWlanSwitchOff);
    CHECK(fail.model.state() == ap::ApctlState::kDisconnected);
    CHECK(fail.model.results().empty());
    fail.clock.advance(kScanUs);
    fail.stage.drain(fail.model);
    CHECK_EQ(fail.stage.flags, 2u); // no SCAN_COMPLETE afterwards
    fail.model.fail_scan(1);        // not scanning: no-op
    CHECK_EQ(fail.model.pending_handler_calls(), std::size_t{0});

    // A source error takes the same path.
    struct Failing final : ap::ScanSource {
        ap::ScanOutcome scan(const ap::ScanRequest &) override { return {err::kTimeout, {}}; }
    } failing;
    mpo::kernel::VirtualClock clock;
    ap::ApctlModel model(clock, failing);
    model.init();
    model.add_handler(kHandlerEntry, kStageObject);
    Stage stage;
    stage.start_scan(model);
    clock.advance(kScanUs);
    stage.drain(model);
    CHECK_EQ(stage.flags, 2u);
    CHECK_EQ(stage.error, err::kTimeout);
    CHECK_EQ(model.scans_completed(), 0u);
}

void test_handlers() {
    Fixture f;
    CHECK_EQ(f.model.add_handler(0x08800000u, 1), 1);
    CHECK_EQ(f.model.add_handler(0x08800010u, 2), 2);
    CHECK_EQ(f.model.del_handler(1), 0);
    CHECK_EQ(f.model.add_handler(0x08800020u, 3), 1); // lowest free id
    f.model.scan_user();
    // One call per handler, in id order.
    std::vector<ap::HandlerCall> calls;
    while (auto c = f.model.pop_handler_call()) calls.push_back(*c);
    CHECK_EQ(calls.size(), std::size_t{3});
    CHECK_EQ(calls[0].handler_id, 0);
    CHECK_EQ(calls[1].handler_id, 1);
    CHECK_EQ(calls[1].entry, 0x08800020u);
    CHECK_EQ(calls[1].arg, 3u);
    CHECK_EQ(calls[2].handler_id, 2);
    // Deleting a handler drops its pending calls.
    f.clock.advance(kScanUs);
    f.model.update();
    CHECK_EQ(f.model.pending_handler_calls(), std::size_t{3});
    CHECK_EQ(f.model.del_handler(2), 0);
    CHECK_EQ(f.model.pending_handler_calls(), std::size_t{2});

    // The limit.
    mpo::kernel::VirtualClock clock;
    ap::VirtualApSource source(config_with_seed(0));
    ap::ApctlModel model(clock, source);
    for (std::size_t i = 0; i < ap::kMaxHandlers; ++i) {
        CHECK_EQ(model.add_handler(0x08800000u, 0), static_cast<std::int32_t>(i));
    }
    CHECK_EQ(model.add_handler(0x08800000u, 0), ap::result(err::kTooManyHandlers));
}

void test_disconnect_and_term() {
    Fixture f;
    f.model.scan_user();
    CHECK_EQ(f.model.disconnect(), 0);
    CHECK(f.model.state() == ap::ApctlState::kDisconnected);
    // The pending SCAN_REQUEST is dropped; DISCONNECT_REQUEST replaces it.
    CHECK(f.model.pop_handler_call() == call(0, kScanning, kDisconnected, kDisconnectRequest));
    CHECK(!f.model.pop_handler_call());
    f.clock.advance(kScanUs);
    f.model.update();
    CHECK(!f.model.pop_handler_call()); // the cancelled scan never completes
    CHECK_EQ(f.model.scans_completed(), 0u);
    // Disconnecting while idle still notifies (PPSSPP).
    f.model.disconnect();
    CHECK(f.model.pop_handler_call() == call(0, kDisconnected, kDisconnected, kDisconnectRequest));

    // Term: handlers, calls and results go; init starts over.
    f.model.scan_user();
    f.clock.advance(kScanUs);
    f.model.update();
    CHECK(!f.model.results().empty());
    CHECK_EQ(f.model.term(), 0);
    CHECK(!f.model.initialized());
    CHECK(f.model.results().empty());
    CHECK_EQ(f.model.pending_handler_calls(), std::size_t{0});
    CHECK_EQ(f.model.del_handler(0), ap::result(err::kGeneric));
    CHECK_EQ(f.model.init(), 0);
    CHECK_EQ(f.model.scans_started(), 0u);
    CHECK_EQ(f.model.add_handler(kHandlerEntry, kStageObject), 0);
}

void test_rotation_through_model() {
    mpo::kernel::VirtualClock clock;
    ap::VirtualApConfig config = config_with_seed(11);
    config.rotation_period_us = 2'000'000;
    config.jitter = 0;
    ap::VirtualApSource source(config);
    ap::ApctlModel model(clock, source);
    model.init();
    model.scan_user(); // completes at 0.7 s: epoch 0
    clock.advance(kScanUs);
    model.update();
    const auto first = model.results();
    model.scan_user(); // completes at 1.4 s: epoch 0
    clock.advance(kScanUs);
    model.update();
    CHECK(model.results() == first);
    model.scan_user(); // completes at 2.1 s: epoch 1
    clock.advance(kScanUs);
    model.update();
    CHECK(model.results() != first);
    CHECK(model.results() == source.generator().access_points(1));
}

void test_os_source_stub() {
    CHECK(ap::make_os_scan_source() == nullptr);
}

} // namespace

int main() {
    test_generator_golden();
    test_generator_stable_and_distinct();
    test_generator_constraints();
    test_generator_jitter();
    test_generator_rotation();
    test_validate();
    test_scan_sequence();
    test_stage_loop();
    test_id_list_layout();
    test_entry_layout();
    test_errors();
    test_handlers();
    test_disconnect_and_term();
    test_rotation_through_model();
    test_os_source_stub();
    std::printf("mpo_apscan_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
