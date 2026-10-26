// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PPSSPP Core/HLE/sceCtrl.h and Core/HLE/sceCtrl.cpp (CtrlData)
// @ v1.20.4 (GPL-2.0-or-later): the PSP button bits and the 16-byte
// SceCtrlData record that sceCtrlReadBuffer* writes to guest memory.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mpo::frontend::ctrl {

inline constexpr std::uint32_t kSelect = 0x0001;
inline constexpr std::uint32_t kL3 = 0x0002; // Not on a PSP (PPSSPP extra).
inline constexpr std::uint32_t kR3 = 0x0004; // Not on a PSP (PPSSPP extra).
inline constexpr std::uint32_t kStart = 0x0008;
inline constexpr std::uint32_t kUp = 0x0010;
inline constexpr std::uint32_t kRight = 0x0020;
inline constexpr std::uint32_t kDown = 0x0040;
inline constexpr std::uint32_t kLeft = 0x0080;
inline constexpr std::uint32_t kLTrigger = 0x0100;
inline constexpr std::uint32_t kRTrigger = 0x0200;
inline constexpr std::uint32_t kL2 = 0x0400; // Not on a PSP (PPSSPP extra).
inline constexpr std::uint32_t kR2 = 0x0800; // Not on a PSP (PPSSPP extra).
inline constexpr std::uint32_t kTriangle = 0x1000;
inline constexpr std::uint32_t kCircle = 0x2000;
inline constexpr std::uint32_t kCross = 0x4000;
inline constexpr std::uint32_t kSquare = 0x8000;
inline constexpr std::uint32_t kHome = 0x00010000;
inline constexpr std::uint32_t kHold = 0x00020000;
inline constexpr std::uint32_t kWlan = 0x00040000;
inline constexpr std::uint32_t kRemoteHold = 0x00080000;
inline constexpr std::uint32_t kVolUp = 0x00100000;
inline constexpr std::uint32_t kVolDown = 0x00200000;
inline constexpr std::uint32_t kScreen = 0x00400000;
inline constexpr std::uint32_t kNote = 0x00800000;

inline constexpr std::uint32_t kMaskDpad = kUp | kDown | kLeft | kRight;
inline constexpr std::uint32_t kMaskAction = kSquare | kTriangle | kCircle | kCross;
inline constexpr std::uint32_t kMaskTrigger = kLTrigger | kRTrigger;
// CTRL_MASK_USER: what sceCtrlReadBuffer* lets a game see.
inline constexpr std::uint32_t kMaskUser =
    kMaskDpad | kMaskAction | kStart | kSelect | kMaskTrigger | kHome | kHold | kWlan |
    kRemoteHold | kVolUp | kVolDown | kScreen | kNote | kL2 | kL3 | kR2 | kR3;

inline constexpr std::uint8_t kAnalogCenter = 128;
inline constexpr int kStickLeft = 0;
inline constexpr int kStickRight = 1; // Not on a PSP; PPSSPP keeps a slot for it.
inline constexpr int kAxisX = 0;
inline constexpr int kAxisY = 1;

// Button name ("cross", "l", ...) for one bit, or "" when unnamed.
[[nodiscard]] std::string_view ButtonName(std::uint32_t bit);
// Bit for a name, 0 when unknown.
[[nodiscard]] std::uint32_t ButtonFromName(std::string_view name);
// "cross|up", or "-" for none.
[[nodiscard]] std::string ButtonsToString(std::uint32_t buttons);

} // namespace mpo::frontend::ctrl

namespace mpo::frontend {

// SceCtrlData as PPSSPP lays it out (CtrlData): timestamp, buttons, then
// analog[stick][axis] (the PSP SDK calls the first two bytes Lx and Ly and the
// other six Rsrv). Axes are 0..255, 128 centred, Y grows downwards.
struct SceCtrlData {
    std::uint32_t timestamp = 0;
    std::uint32_t buttons = 0;
    std::uint8_t analog[2][2] = {{ctrl::kAnalogCenter, ctrl::kAnalogCenter},
                                 {ctrl::kAnalogCenter, ctrl::kAnalogCenter}};
    std::uint8_t reserved[4] = {};

    // The 16 bytes as they go to guest memory (little-endian).
    [[nodiscard]] std::array<std::uint8_t, 16> Serialize() const;
    void SerializeTo(std::span<std::uint8_t, 16> out) const;
};
static_assert(sizeof(SceCtrlData) == 16);

// One trace line: "t=<timestamp> buttons=0x00004000 [cross] lx=128 ly=128 rx=128 ry=128".
[[nodiscard]] std::string FormatCtrl(const SceCtrlData &data);

} // namespace mpo::frontend
