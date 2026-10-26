// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scanning half of sceNetApctl, without the HLE (F20a).
//
// The model owns the Apctl state, the handlers registered with
// sceNetApctlAddHandler, the scan in progress and the results of the last
// scan. It never calls guest code and never touches PSP memory on its own:
//   - handler notifications are queued as HandlerCall records; the HLE (F20b)
//     runs each one as a MIPS callback from its own Apctl thread;
//   - the BSS list and entries are written into spans the HLE resolves.
//
// Contract with MPO (docs/analysis/apscan.md):
//   - the stage's handler only reacts when newState is DISCONNECTED: event
//     SCAN_COMPLETE marks the scan done, event ERROR stores the error code;
//   - the stage clears its flags right after sceNetApctlScanUser returns, so
//     no notification may reach it before that call returns. The model never
//     delivers anything itself; the HLE drains pop_handler_call() from the
//     Apctl thread only.
// The event shapes follow JPCSP (state changes before the handler runs;
// SCAN_COMPLETE goes SCANNING -> DISCONNECTED), which runs MPO's AP Scan.
//
// Not thread-safe: the PSP has one core and the HLE calls in from fibers.

#pragma once

#include "apscan/access_point.hpp"
#include "apscan/apctl_types.hpp"
#include "apscan/scan_source.hpp"
#include "kernel/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace mpo::apscan {

struct ApctlModelConfig {
    // PSP time between sceNetApctlScanUser and SCAN_COMPLETE. JPCSP uses
    // 700 ms (SCAN_DURATION_MILLIS); the firmware's time is unknown. MPO scans
    // again as soon as a scan completes, so this sets how often its signal
    // bar gets a new target.
    std::uint64_t scan_duration_us = 700'000;
    // Position of the PSP's WLAN switch. When off, sceNetApctlScanUser fails
    // with error::kWlanSwitchOff, like PPSSPP with Wi-Fi disabled.
    bool wlan_switch_on = true;
};

// One handler invocation: handler(old_state, new_state, event, error, arg).
// In the PSP calling convention the five arguments go in $a0-$a3 and $t0.
struct HandlerCall {
    std::int32_t handler_id = 0;
    std::uint32_t entry = 0; // guest address of the handler
    std::uint32_t old_state = 0;
    std::uint32_t new_state = 0;
    std::uint32_t event = 0;
    std::uint32_t error = 0;
    std::uint32_t arg = 0; // the pArg given to sceNetApctlAddHandler

    friend bool operator==(const HandlerCall &, const HandlerCall &) = default;
};

class ApctlModel {
public:
    // `clock` and `source` must outlive the model.
    ApctlModel(const kernel::Clock &clock, ScanSource &source, const ApctlModelConfig &config = {});

    // sceNetApctlInit / sceNetApctlTerm. Term also drops the handlers, the
    // pending calls and the results.
    std::int32_t init();
    std::int32_t term();
    [[nodiscard]] bool initialized() const { return initialized_; }

    // sceNetApctlAddHandler: returns the lowest free id (from 0) or
    // error::kTooManyHandlers. The HLE checks that `entry` is a valid address.
    std::int32_t add_handler(std::uint32_t entry, std::uint32_t arg);
    // sceNetApctlDelHandler: also drops that handler's pending calls.
    std::int32_t del_handler(std::int32_t handler_id);

    // sceNetApctlScanUser: DISCONNECTED -> SCANNING, queues SCAN_REQUEST and
    // completes the scan scan_duration_us later (see update()).
    std::int32_t scan_user();
    // sceNetApctlDisconnect: cancels a scan, drops pending calls and queues
    // DISCONNECT_REQUEST (PPSSPP does the same; Phantasy Star Portable 2 waits
    // for it).
    std::int32_t disconnect();
    // sceNetApctlGetState.
    [[nodiscard]] ApctlState state() const { return state_; }

    void set_wlan_switch(bool on) { config_.wlan_switch_on = on; }
    [[nodiscard]] bool wlan_switch() const { return config_.wlan_switch_on; }

    // Ends the scan in progress with an ERROR event carrying `error_code`
    // (for example when the host loses its Wi-Fi). No-op if not scanning.
    void fail_scan(std::uint32_t error_code);

    // Completes the scan in progress if its time has come: asks the source,
    // keeps the results and queues SCAN_COMPLETE (or ERROR).
    void update();
    // When update() next has work, or "now" if handler calls are pending;
    // nullopt when idle. The HLE's Apctl thread sleeps until then.
    [[nodiscard]] std::optional<std::uint64_t> next_deadline_us() const;
    // Next handler call to run, in order.
    std::optional<HandlerCall> pop_handler_call();
    [[nodiscard]] std::size_t pending_handler_calls() const { return calls_.size(); }

    // sceNetApctlGetBSSDescIDListUser. `buffer` is the guest buffer (the
    // caller's *sizeAddr bytes at `buffer_address`, empty for a null pointer).
    // Writes as many 8-byte entries {next, id} as fit, the last written one
    // with next = 0, and returns the size the whole list needs, which the HLE
    // stores back in *sizeAddr (PPSSPP and JPCSP do the same).
    std::uint32_t write_bss_id_list(std::uint32_t buffer_address,
                                    std::span<std::uint8_t> buffer) const;
    // sceNetApctlGetBSSDescEntryUser: writes bss_desc_entry_size(info_id)
    // bytes of entry `entry_id` of the last scan into `out`.
    std::int32_t get_bss_desc_entry(std::int32_t entry_id, std::int32_t info_id,
                                    std::span<std::uint8_t> out) const;
    // Bytes an infoId writes; 0 for an unknown one.
    [[nodiscard]] static std::uint32_t bss_desc_entry_size(std::int32_t info_id);

    // Results of the last completed scan (entry id = index).
    [[nodiscard]] const std::vector<AccessPoint> &results() const { return results_; }
    [[nodiscard]] std::uint64_t scans_started() const { return scans_started_; }
    [[nodiscard]] std::uint64_t scans_completed() const { return scans_completed_; }

private:
    struct Handler {
        std::uint32_t entry;
        std::uint32_t arg;
    };

    // Moves to `new_state` and queues `event` for every handler.
    void transition(ApctlState new_state, ApctlEvent event, std::uint32_t error);

    const kernel::Clock &clock_;
    ScanSource &source_;
    ApctlModelConfig config_;
    bool initialized_ = false;
    ApctlState state_ = ApctlState::kDisconnected;
    std::map<std::int32_t, Handler> handlers_;
    std::deque<HandlerCall> calls_;
    std::optional<std::uint64_t> scan_deadline_us_;
    std::vector<AccessPoint> results_;
    std::uint64_t scans_started_ = 0;
    std::uint64_t scans_completed_ = 0;
};

} // namespace mpo::apscan
