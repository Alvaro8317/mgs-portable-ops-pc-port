// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceUmdUser and scePower (F09), plus the stubs the boot needs before the
// game reads any data, registered as HleKind::kStub until their phase:
//
//   - sceOpenPSIDGetOpenPSID: PPSSPP's fixed id (F20b);
//   - sceUtilityGetSystemParamInt and sceImposeSetLanguageMode: PPSSPP's
//     default system settings, English and Cross to confirm (F18b).
//   - sceAudio channels (F16b): reserve, release, data length and volume
//     keep PPSSPP's checks, and a blocking output waits as long as its samples
//     last at 44.1 kHz, without mixing anything. Without that wait the sound
//     threads (priority 0x10/0x11) spin and starve every thread below them.
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
constexpr std::uint32_t kErrInvalidSystemParam = 0x80110103u;

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

constexpr std::uint32_t kAudioChannels = 8u;
constexpr std::uint32_t kAudioSampleMax = 65536u - 64u;
constexpr std::uint32_t kAudioFormatStereo = 0x00u;
constexpr std::uint32_t kAudioFormatMono = 0x10u;
constexpr std::uint32_t kErrAudioChannelNotInit = 0x80260001u;
constexpr std::uint32_t kErrAudioInvalidChannel = 0x80260003u;
constexpr std::uint32_t kErrAudioNoChannels = 0x80260005u;
constexpr std::uint32_t kErrAudioNotAligned = 0x80260006u;
constexpr std::uint32_t kErrAudioInvalidFormat = 0x80260007u;
constexpr std::uint32_t kErrAudioChannelNotReserved = 0x80260008u;

constexpr std::uint32_t kLanguageEnglish = 1u;
constexpr std::uint32_t kButtonCross = 1u;

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
    stub("sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt", [&k](Ctx &c) {
        std::uint32_t value = 0;
        switch (arg(c, 0)) {
        case 2: // Ad hoc channel: automatic.
            k.memory().write_u32(arg(c, 1), 0u);
            return ret(c, 0x800ADF4u);
        case 3:  // WLAN power save: off.
        case 4:  // Date format: YYYYMMDD.
        case 5:  // Time format: 24 h.
        case 6:  // Time zone: 0.
        case 7:  // Daylight savings: standard.
        case 10: // Parental level: 0.
            value = 0u;
            break;
        case 8:
            value = kLanguageEnglish;
            break;
        case 9:
            value = kButtonCross;
            break;
        default:
            return ret(c, kErrInvalidSystemParam);
        }
        k.memory().write_u32(arg(c, 1), value);
        ret(c, 0u);
    });
    stub("sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode", [](Ctx &c) { ret(c, 0u); });

    struct AudioChannel {
        bool reserved{};
        std::uint32_t samples{};
    };
    auto channels = std::make_shared<std::array<AudioChannel, kAudioChannels>>();
    stub("sceAudio", 0x5EC81C55u, "sceAudioChReserve", [channels](Ctx &c) {
        auto channel = static_cast<std::int32_t>(arg(c, 0));
        const std::uint32_t samples = arg(c, 1);
        const std::uint32_t format = arg(c, 2);
        if (channel < 0) {
            // GetFreeChannel: from the top, never channel 0.
            channel = -1;
            for (std::uint32_t i = kAudioChannels - 1u; i > 0u && channel < 0; --i)
                if (!(*channels)[i].reserved) channel = static_cast<std::int32_t>(i);
            if (channel < 0) return ret(c, kErrAudioNoChannels);
        }
        if (static_cast<std::uint32_t>(channel) >= kAudioChannels)
            return ret(c, kErrAudioInvalidChannel);
        if ((samples & 63u) != 0u || samples == 0u || samples > kAudioSampleMax)
            return ret(c, kErrAudioNotAligned);
        if (format != kAudioFormatStereo && format != kAudioFormatMono)
            return ret(c, kErrAudioInvalidFormat);
        AudioChannel &slot = (*channels)[static_cast<std::size_t>(channel)];
        if (slot.reserved) return ret(c, kErrAudioInvalidChannel);
        slot = AudioChannel{true, samples};
        ret(c, static_cast<std::uint32_t>(channel));
    });
    stub("sceAudio", 0x6FC46853u, "sceAudioChRelease", [channels](Ctx &c) {
        const std::uint32_t channel = arg(c, 0);
        if (channel >= kAudioChannels) return ret(c, kErrAudioInvalidChannel);
        if (!(*channels)[channel].reserved) return ret(c, kErrAudioChannelNotInit);
        (*channels)[channel] = AudioChannel{};
        ret(c, 0u);
    });
    stub("sceAudio", 0xCB2E439Eu, "sceAudioSetChannelDataLen", [channels](Ctx &c) {
        const std::uint32_t channel = arg(c, 0);
        const std::uint32_t samples = arg(c, 1);
        if (channel >= kAudioChannels) return ret(c, kErrAudioInvalidChannel);
        if (!(*channels)[channel].reserved) return ret(c, kErrAudioChannelNotInit);
        if ((samples & 63u) != 0u || samples == 0u || samples > kAudioSampleMax)
            return ret(c, kErrAudioNotAligned);
        (*channels)[channel].samples = samples;
        ret(c, 0u);
    });
    stub("sceAudio", 0xB7E1D8E7u, "sceAudioChangeChannelVolume", [channels](Ctx &c) {
        const std::uint32_t channel = arg(c, 0);
        if (channel >= kAudioChannels) return ret(c, kErrAudioInvalidChannel);
        if (!(*channels)[channel].reserved) return ret(c, kErrAudioChannelNotInit);
        ret(c, 0u);
    });
    const auto output = [&k, channels](Ctx &c) {
        const std::uint32_t channel = arg(c, 0);
        if (channel >= kAudioChannels) return ret(c, kErrAudioInvalidChannel);
        if (!(*channels)[channel].reserved) return ret(c, kErrAudioChannelNotReserved);
        const std::uint32_t samples = (*channels)[channel].samples;
        // The time the samples take to play; nothing is mixed (F16b).
        ret(c, k.delay_result(
                   0u, static_cast<std::uint32_t>(std::uint64_t{samples} * 1'000'000u / 44'100u)));
    };
    stub("sceAudio", 0x13F592BCu, "sceAudioOutputPannedBlocking", output);
    stub("sceAudio", 0x136CAF51u, "sceAudioOutputBlocking", output);
}

} // namespace mpo::hle
