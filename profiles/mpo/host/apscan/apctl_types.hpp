// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceNetApctl values used by the AP Scan model (F20a). The numbers are the
// firmware's; they come from PPSSPP Core/HLE/sceNetApctl.h and ErrorCodes.h @
// v1.20.4 and match JPCSP's sceNetApctl.java (commit 00ac4d6). What MPO itself
// reads is in docs/analysis/apscan.md.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mpo::apscan {

// PSP_NET_APCTL_STATE_*.
enum class ApctlState : std::uint32_t {
    kDisconnected = 0,
    kScanning = 1,
    kJoining = 2,
    kGettingIp = 3,
    kGotIp = 4,
    kEapAuth = 5,
    kKeyExchange = 6,
};

// PSP_NET_APCTL_EVENT_*: the third argument of the handler.
enum class ApctlEvent : std::uint32_t {
    kConnectRequest = 0,
    kScanRequest = 1,
    kScanComplete = 2,
    kEstablished = 3,
    kGetIp = 4,
    kDisconnectRequest = 5,
    kError = 6,
    kInfo = 7,
    kEapAuth = 8,
    kKeyExchange = 9,
    kReconnect = 10,
};

// PSP_NET_APCTL_DESC_*: the infoId of sceNetApctlGetBSSDescEntryUser.
enum class BssDescInfo : std::int32_t {
    kBssid = 0,          // 6 bytes
    kSsid = 1,           // 32 bytes, zero padded
    kSsidLength = 2,     // u32
    kChannel = 3,        // u8 (PPSSPP's guess; MPO never asks)
    kSignalStrength = 4, // u8, percent
    kSecurity = 5,       // u32, SecurityType (MPO never asks)
};

// PSP_NET_APCTL_INFO_SECURITY_TYPE_*.
enum class SecurityType : std::uint32_t {
    kNone = 0,
    kWep = 1,
    kWpaTkip = 2,
    kUnsupported = 3,
    kWpaAes = 4,
};

inline constexpr std::size_t kBssidSize = 6;
inline constexpr std::size_t kSsidSize = 32;
// One entry of the sceNetApctlGetBSSDescIDListUser list: u32 next, u32 id.
inline constexpr std::size_t kIdListEntrySize = 8;
// PPSSPP's MAX_APCTL_HANDLERS; the firmware's limit is unknown.
inline constexpr std::size_t kMaxHandlers = 32;

namespace error {

inline constexpr std::uint32_t kAlreadyInitialized = 0x80410A01u;
inline constexpr std::uint32_t kInvalidCode = 0x80410A02u;
inline constexpr std::uint32_t kNotDisconnected = 0x80410A04u;
inline constexpr std::uint32_t kNotInBss = 0x80410A05u;
inline constexpr std::uint32_t kWlanSwitchOff = 0x80410A06u;
inline constexpr std::uint32_t kInvalidId = 0x80410A09u;
inline constexpr std::uint32_t kTimeout = 0x80410A0Bu;
// PPSSPP borrows the adhocctl code when there are too many handlers.
inline constexpr std::uint32_t kTooManyHandlers = 0x80410B12u;
// Plain -1: what PPSSPP and JPCSP return for an unknown handler id or a bad
// pointer.
inline constexpr std::uint32_t kGeneric = 0xFFFFFFFFu;

} // namespace error

// A syscall result as the guest sees it in $v0.
[[nodiscard]] constexpr std::int32_t result(std::uint32_t code) {
    return static_cast<std::int32_t>(code);
}

} // namespace mpo::apscan
