// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUmdUser and scePower (F09), plus the stubs the boot needs before the
// game reads any data, registered as HleKind::kStub until their phase:
//
//   - sceOpenPSIDGetOpenPSID: PPSSPP's fixed id (F20b).
//
// sceUtilityGetSystemParamInt and sceImposeSetLanguageMode moved to
// hle_utility.cpp in F18b.
//
// sceAudio left this file for hle_audio.cpp in F16b.
//
// Ported from PPSSPP Core/HLE/sceUmd.cpp, scePower.cpp, sceOpenPSID.cpp,
// sceUtility.cpp and sceImpose.cpp @ v1.20.4.

#include <array>
#include <memory>

#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

constexpr std::uint32_t kErrInvalidArgument = 0x80010016u;
constexpr std::uint32_t kErrPrivRequired = 0x80000023u;

// The disc is inserted, ready and (umdActivated starts true) readable.
constexpr std::uint32_t kUmdNotPresent = 0x01u;
constexpr std::uint32_t kUmdPresent = 0x02u;
constexpr std::uint32_t kUmdNotReady = 0x08u;
constexpr std::uint32_t kUmdReady = 0x10u;
constexpr std::uint32_t kUmdReadable = 0x20u;
constexpr std::uint32_t kUmdState = kUmdPresent | kUmdReady | kUmdReadable;
constexpr std::uint32_t kUmdAllowWait =
    kUmdNotPresent | kUmdPresent | kUmdNotReady | kUmdReady | kUmdReadable;

constexpr std::uint32_t kPowerTakenSlot = 0x80000020u;
constexpr std::uint32_t kPowerSlotsFull = 0x80000022u;
constexpr std::uint32_t kPowerEmptySlot = 0x80000025u;
constexpr std::uint32_t kPowerInvalidCallback = 0x80000100u;
constexpr std::uint32_t kPowerInvalidSlot = 0x80000102u;
constexpr int kPowerSlots = 16;
constexpr int kPowerSlotsPrivate = 32;
// AC power, a battery, full.
constexpr std::uint32_t kPowerState = 0x00001000u | 0x00000080u | 0x00000064u;

constexpr std::array<std::uint8_t, 16> kOpenPsid = {0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0,
                                                    0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9D};

} // namespace

void register_system_hle(System &system) {
    kernel::Kernel &k = system.kernel();
    HleTable &t = system.hle();
    GameClock &clock = system.clock();
    const auto def = [&t](const char *lib, std::uint32_t nid, const char *name,
                          HleTable::Handler handler) {
        t.add(lib, nid, name, HleKind::kImplemented, std::move(handler));
    };
    const auto stub = [&t](const char *lib, std::uint32_t nid, const char *name,
                           HleTable::Handler handler) {
        t.add(lib, nid, name, HleKind::kStub, std::move(handler));
    };

    // ---- sceUmdUser ----
    def("sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat", [](Ctx &c) { ret(c, kUmdState); });
    def("sceUmdUser", 0xC6183D47u, "sceUmdActivate",
        [](Ctx &c) { ret(c, arg(c, 0) < 1u || arg(c, 0) > 2u ? kErrInvalidArgument : 0u); });
    // The state never lacks a waitable bit but NOT_PRESENT/NOT_READY, which a
    // game does not wait for, so these never block.
    def("sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat", [&k, &clock](Ctx &c) {
        if ((arg(c, 0) & kUmdAllowWait) == 0u) return ret(c, kErrInvalidArgument);
        if (!k.dispatch_enabled()) return ret(c, kernel::error::kCanNotWait);
        if (k.in_interrupt()) return ret(c, kernel::error::kIllegalContext);
        clock.eat_cycles(520u);
        ret(c, 0u);
    });
    def("sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB", [&k, &clock](Ctx &c) {
        if ((arg(c, 0) & kUmdAllowWait) == 0u) return ret(c, kErrInvalidArgument);
        if (!k.dispatch_enabled()) return ret(c, kernel::error::kCanNotWait);
        if (k.in_interrupt()) return ret(c, kernel::error::kIllegalContext);
        clock.eat_cycles(520u);
        (void)k.check_callback();
        ret(c, 0u);
        k.preemption_point();
    });

    // ---- scePower ----
    auto slots = std::make_shared<std::array<std::uint32_t, kPowerSlots>>();
    def("scePower", 0x04B7766Eu, "scePowerRegisterCallback", [&k, slots](Ctx &c) {
        const auto slot = static_cast<std::int32_t>(arg(c, 0));
        const std::uint32_t callback = arg(c, 1);
        if (slot < -1 || slot >= kPowerSlotsPrivate) return ret(c, kPowerInvalidSlot);
        if (slot >= kPowerSlots) return ret(c, kErrPrivRequired);
        if (callback == 0u) return ret(c, kPowerInvalidCallback);
        std::int32_t taken = -1;
        if (slot == -1) {
            for (std::int32_t i = 0; i < kPowerSlots && taken == -1; ++i) {
                if ((*slots)[static_cast<std::size_t>(i)] == 0u) {
                    (*slots)[static_cast<std::size_t>(i)] = callback;
                    taken = i;
                }
            }
            if (taken == -1) return ret(c, kPowerSlotsFull);
        } else {
            if ((*slots)[static_cast<std::size_t>(slot)] != 0u) return ret(c, kPowerTakenSlot);
            (*slots)[static_cast<std::size_t>(slot)] = callback;
            taken = 0;
        }
        k.notify_callback_from_host(static_cast<kernel::Uid>(callback), kPowerState);
        ret(c, static_cast<std::uint32_t>(taken));
    });
    def("scePower", 0xDFA8BAF8u, "scePowerUnregisterCallback", [slots](Ctx &c) {
        const auto slot = static_cast<std::int32_t>(arg(c, 0));
        if (slot < 0 || slot >= kPowerSlotsPrivate) return ret(c, kPowerInvalidSlot);
        if (slot >= kPowerSlots) return ret(c, kErrPrivRequired);
        if ((*slots)[static_cast<std::size_t>(slot)] == 0u) return ret(c, kPowerEmptySlot);
        (*slots)[static_cast<std::size_t>(slot)] = 0u;
        ret(c, 0u);
    });

    // ---- Boot stubs ----
    stub("sceOpenPSID", 0xC69BEBCEu, "sceOpenPSIDGetOpenPSID", [&k](Ctx &c) {
        (void)k.memory().write_bytes(arg(c, 0), kOpenPsid);
        ret(c, 0u);
    });
}

} // namespace mpo::hle
