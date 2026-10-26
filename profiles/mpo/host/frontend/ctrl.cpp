// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctrl.h"

#include <cstdio>
#include <cstring>

namespace mpo::frontend::ctrl {
namespace {

struct NamedButton {
    std::uint32_t bit;
    std::string_view name;
};

constexpr NamedButton kButtonNames[] = {
    {kSelect, "select"},
    {kL3, "l3"},
    {kR3, "r3"},
    {kStart, "start"},
    {kUp, "up"},
    {kRight, "right"},
    {kDown, "down"},
    {kLeft, "left"},
    {kLTrigger, "l"},
    {kRTrigger, "r"},
    {kL2, "l2"},
    {kR2, "r2"},
    {kTriangle, "triangle"},
    {kCircle, "circle"},
    {kCross, "cross"},
    {kSquare, "square"},
    {kHome, "home"},
    {kHold, "hold"},
    {kWlan, "wlan"},
    {kRemoteHold, "remote_hold"},
    {kVolUp, "vol_up"},
    {kVolDown, "vol_down"},
    {kScreen, "screen"},
    {kNote, "note"},
};

} // namespace

std::string_view ButtonName(std::uint32_t bit) {
    for (const NamedButton &b : kButtonNames) {
        if (b.bit == bit) return b.name;
    }
    return {};
}

std::uint32_t ButtonFromName(std::string_view name) {
    for (const NamedButton &b : kButtonNames) {
        if (b.name == name) return b.bit;
    }
    return 0;
}

std::string ButtonsToString(std::uint32_t buttons) {
    std::string out;
    for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((buttons & bit) == 0) continue;
        if (!out.empty()) out += '|';
        const std::string_view name = ButtonName(bit);
        if (name.empty()) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%x", bit);
            out += hex;
        } else {
            out += name;
        }
    }
    return out.empty() ? "-" : out;
}

} // namespace mpo::frontend::ctrl

namespace mpo::frontend {

void SceCtrlData::SerializeTo(std::span<std::uint8_t, 16> out) const {
    for (int i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(timestamp >> (8 * i));
        out[static_cast<std::size_t>(4 + i)] = static_cast<std::uint8_t>(buttons >> (8 * i));
    }
    out[8] = analog[0][0];
    out[9] = analog[0][1];
    out[10] = analog[1][0];
    out[11] = analog[1][1];
    std::memcpy(out.data() + 12, reserved, 4);
}

std::array<std::uint8_t, 16> SceCtrlData::Serialize() const {
    std::array<std::uint8_t, 16> out{};
    SerializeTo(out);
    return out;
}

std::string FormatCtrl(const SceCtrlData &data) {
    char head[48];
    std::snprintf(head, sizeof(head), "t=%u buttons=0x%08x [", data.timestamp, data.buttons);
    char tail[48];
    std::snprintf(tail, sizeof(tail), "] lx=%u ly=%u rx=%u ry=%u", data.analog[0][0],
                  data.analog[0][1], data.analog[1][0], data.analog[1][1]);
    return head + ctrl::ButtonsToString(data.buttons) + tail;
}

} // namespace mpo::frontend
