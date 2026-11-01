// SPDX-License-Identifier: GPL-3.0-or-later
//
// The network of the PSP as MPO sees it (F20b): the AP Scan over the virtual
// access points of F20a, and everything else answering "nobody around, no
// network" cleanly.
//
//   - sceNetApctl: ApctlModel (apscan/apctl_model.hpp) on the game clock. Its
//     handler calls run on "ApctlThread", a PSP thread that sceNetApctlInit
//     creates with the game's stack size and priority, like PPSSPP's HLE loop
//     (CpuBridge host thread). The thread sleeps until the model's next
//     deadline and is woken by ScanUser, never delivering anything inside it
//     (docs/analysis/apscan.md §9). The APs come from VirtualApSource with the
//     epoch taken from the local RTC time, so the set changes once a real day
//     (and with NetConfig::access_points.epoch_offset, "go somewhere else").
//     Connecting to an AP is not supported: GetInfo answers NOT_IN_BSS.
//   - sceWlanDrv: the switch is on (the AP Scan needs it) and the MAC is
//     NetConfig::mac. mgp_main writes the MAC into every soldier it creates
//     and compares it later (0x0883914C, 0x08838CA8), so it has to stay the
//     same across sessions.
//   - sceNetAdhocctl and sceNetAdhoc: alone, as on a PSP with nobody in
//     range. A scan finds no group, a created group has no peers, a join
//     times out, and PDP sockets never receive. Handler calls run on
//     "AdhocctlThread" like Apctl's. Ptp and Discover stay unimplemented
//     (only the apscan stage imports them and it never calls them [?]).
//   - sceNetInet, sceNetResolver, sceHttp, sceSsl: init and term succeed;
//     sockets and name lookups fail (ENETDOWN, NO_DNS_SERVER). The game only
//     gets there after sceUtilityNetconf succeeds, which it never does
//     (hle_utility.cpp).
//   - sceUsb and sceUsbGps: no receiver. The USB bus starts but nothing is
//     connected, and the GPS stays off.
//
// Ported from PPSSPP Core/HLE/sceNet.cpp, sceNetAdhoc.cpp, sceNetInet.cpp,
// sceNetResolver.cpp, sceHttp.cpp, sceUsb.cpp and sceUsbGps.cpp @ v1.20.4
// where they apply (argument checks, error codes, the 200 us of
// sceWlanGetEtherAddr); the Apctl events follow JPCSP (apctl_model.hpp).

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "apscan/apctl_model.hpp"
#include "apscan/scan_source.hpp"
#include "apscan/virtual_ap.hpp"
#include "kernel/kernel.hpp"

namespace mpo::hle {

class System;

struct NetConfig {
    // The PSP's MAC address. The low two bits of the first byte are clear
    // (unicast, universally administered), as PPSSPP keeps them.
    std::array<std::uint8_t, 6> mac = {0x00, 0x4D, 0x50, 0x4F, 0x50, 0x43};
    // Position of the WLAN switch. Off, the AP Scan says so and leaves.
    bool wlan_switch = true;
    // The virtual APs (seed, count, signal, and the "place": epoch_offset).
    // rotation_period_us is in real time (the RTC), not PSP time.
    apscan::VirtualApConfig access_points;
};

// Parses "aa:bb:cc:dd:ee:ff" (or with '-').
[[nodiscard]] bool parse_mac(const std::string &text, std::array<std::uint8_t, 6> *out);

class Net {
public:
    struct Stats {
        std::uint64_t ap_scans = 0;      // sceNetApctlScanUser that started a scan
        std::uint64_t ap_scans_done = 0; // scans that completed
        std::uint64_t apctl_handler_calls = 0;
        std::uint64_t adhoc_scans = 0;
        std::uint64_t adhocctl_handler_calls = 0;
        std::uint64_t bss_entries_read = 0; // successful GetBSSDescEntryUser
    };

    Net(System &system, const NetConfig &config);
    ~Net();
    Net(const Net &) = delete;
    Net &operator=(const Net &) = delete;

    void register_hle();

    [[nodiscard]] const NetConfig &config() const noexcept { return config_; }
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    [[nodiscard]] const apscan::ApctlModel &apctl() const noexcept { return *apctl_; }
    // The APs a scan would see now (for the report and tests).
    [[nodiscard]] std::vector<apscan::AccessPoint> visible_access_points() const;

private:
    // A PSP thread that runs host code: it delivers queued handler calls and
    // sleeps until the next deadline (or a kick).
    struct EventThread {
        const char *name = "";
        std::uint32_t entry = 0; // the host thread entry (CpuBridge)
        kernel::Uid uid = 0;
        bool running = false;
        bool waiting = false;
    };
    struct AdhocEvent {
        std::uint64_t due_us = 0;
        std::uint32_t event = 0;
        std::uint32_t error = 0;
        std::uint32_t new_state = 0;
    };
    struct AdhocHandler {
        std::uint32_t entry = 0;
        std::uint32_t arg = 0;
    };

    std::uint32_t start_thread(EventThread &thread, std::uint32_t priority,
                               std::uint32_t stack_size);
    void stop_thread(EventThread &thread);
    void kick(EventThread &thread);
    void wait_until(EventThread &thread, std::optional<std::uint64_t> deadline_us);
    std::uint32_t apctl_thread_main();
    std::uint32_t adhocctl_thread_main();
    [[nodiscard]] std::optional<std::uint64_t> adhocctl_deadline() const;
    void queue_adhocctl_event(std::uint64_t delay_us, std::uint32_t event, std::uint32_t error,
                              std::uint32_t new_state);

    System &system_;
    NetConfig config_;
    std::unique_ptr<apscan::ScanSource> source_;
    std::unique_ptr<apscan::ApctlModel> apctl_;
    EventThread apctl_thread_{"ApctlThread"};
    EventThread adhocctl_thread_{"AdhocctlThread"};

    bool net_inited_ = false;
    bool inet_inited_ = false;
    bool resolver_inited_ = false;
    std::set<std::int32_t> resolvers_;
    std::int32_t next_resolver_ = 1;
    std::uint32_t inet_errno_ = 0;

    bool adhoc_inited_ = false;
    bool adhocctl_inited_ = false;
    std::uint32_t adhocctl_state_ = 0;
    std::map<std::int32_t, AdhocHandler> adhocctl_handlers_;
    std::deque<AdhocEvent> adhocctl_events_;
    std::set<std::int32_t> pdp_sockets_;

    bool usb_started_ = false;
    bool usb_activated_ = false;
    Stats stats_;
};

} // namespace mpo::hle
