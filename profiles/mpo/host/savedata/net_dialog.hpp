// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUtilityNetconf and sceUtilityHtmlViewer without the HLE and without
// drawing (F20b): the port has no infrastructure network, so both dialogs
// only say so and wait for the player to cancel.
// Ported from the "wireless switch off" path of PPSSPP
// Core/Dialog/PSPNetconfDialog.cpp @ v1.20.4: a message, the cancel button,
// and on cancel common.result = ABORT (what PPSSPP gives, which Phantasy Star
// Portable 2 accepts). The firmware's real result there is unknown [?].
//
// MPO opens Netconf with netAction 3 (CONNECT_APNET_LAST) before going online
// (mgp_main 0x0885D7A4), and the HTML viewer for its web links (0x0889925C).
// Neither is needed to play.

#pragma once

#include "kernel/guest_memory.hpp"
#include "savedata/dialog.hpp"
#include "savedata/psp_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mpo::savedata {

struct NetDialogView {
    std::string title;   // the banner ("Network Connection", "Internet Browser")
    std::string message; // why it cannot continue
    std::uint32_t fade = 0;
    std::uint32_t cancel_button = button::kCross;
};

class NetDialog final : public Dialog {
public:
    // PPSSPP's NET_INIT_DELAY_US and NET_SHUTDOWN_DELAY_US.
    static constexpr std::int64_t kInitDelayUs = 200'000;
    static constexpr std::int64_t kShutdownDelayUs = 200'000;
    // The request bytes kept and written back (Netconf's is 0x44; the HTML
    // viewer's is larger, but only its common part changes).
    static constexpr std::uint32_t kMaxRequestSize = 0x400;

    NetDialog(DialogType kind, kernel::Clock &clock, std::string title, std::string message)
        : Dialog(kind, clock), title_(std::move(title)), message_(std::move(message)) {}

    // sceUtility{Netconf,HtmlViewer}InitStart. Returns 0 or a firmware error.
    int init(kernel::GuestMemory &memory, std::uint32_t address);
    // ...Update.
    int update(kernel::GuestMemory &memory, int anim_speed, const ButtonState &buttons);
    // ...ShutdownStart.
    int shutdown(bool force = false);

    [[nodiscard]] NetDialogView view() const;
    // netAction of a Netconf request (0 for the HTML viewer).
    [[nodiscard]] std::int32_t net_action() const;
    [[nodiscard]] std::int32_t result() const;

private:
    [[nodiscard]] DialogCommon common() const;
    void set_result(std::int32_t result);

    std::string title_;
    std::string message_;
    std::uint32_t address_ = 0;
    std::vector<std::uint8_t> request_;
};

} // namespace mpo::savedata
