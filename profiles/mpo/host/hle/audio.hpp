// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio (F16b): sceAudio, sceSasCore and sceAtrac3plus on mpo_audio (F16a),
// with the HLE glue of PPSSPP Core/HLE/sceAudio.cpp, __sceAudio.cpp,
// sceSas.cpp and sceAtrac.cpp @ v1.20.4.
//
// - sceAudio: the channels of AudioChannels. A blocking output that finds
//   samples queued waits (WAITTYPE_AUDIOCHANNEL) until the mixer has consumed
//   them; Audio is the AudioWaitHost that puts the thread to sleep and wakes
//   it on the kernel.
// - The hardware mixer: a kernel event every 64 samples at 44.1 kHz
//   (PPSSPP's audioIntervalCycles, 322176 CPU cycles) pops one block from
//   every reserved channel and hands the mix to set_output() (mpo_boot plays
//   it with SDL3 and writes the WAV dump). It starts at the first reserved
//   channel, on the timeline it would have had since boot, so the kernel
//   still goes idle when nothing plays (the HLE tests).
// - sceSasCore: SasCore mixes on the calling thread and the thread is delayed
//   EstimateMixUs() after a successful __sceSasCore(WithMix), like PPSSPP's
//   sasMixEvent without the separate SAS thread.
// - sceAtrac3plus: AtracManager over the BSS that
//   sceUtilityLoadAvModule(ATRAC3PLUS) reserves (System::load_utility_module
//   calls notify_atrac_module); each call is delayed LastDelayUs() when
//   PPSSPP delays it (hleDelayResult). SAS ATRAC3 voices go through the same
//   manager (SetAtracBridge).

#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "atrac_manager.h"
#include "audio_channels.h"
#include "guest_memory.h"
#include "kernel/kernel.hpp"
#include "sas_core.h"

namespace mpo::hle {

class System;

class Audio final : public audio::AudioWaitHost {
public:
    explicit Audio(System &system);
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;

    void register_hle();

    // sceUtilityLoadAvModule / UnloadAvModule of PSP_AV_MODULE_ATRAC3PLUS:
    // PPSSPP imitates libatrac3plus 0x105 with a 0x67C byte BSS at the start
    // of the module's memory.
    void notify_atrac_module(bool loaded, std::uint32_t address);

    // Every mixed block: kHwBlockSize stereo frames, not clamped (PPSSPP's
    // mixBuffer, which it hands to System_AudioPushSamples and clamps for the
    // WAV dump). Runs on the scheduler path, inside a kernel event.
    using Output = std::function<void(const std::int32_t *mix, int frames)>;
    void set_output(Output output) { output_ = std::move(output); }

    struct Stats {
        std::uint64_t blocks = 0;         // Mixer updates (64 frames each).
        std::uint64_t audible_blocks = 0; // Blocks with a non-zero sample.
        std::uint64_t first_block_us = 0; // Virtual time of the first update.
        std::uint64_t blocking_waits = 0; // Blocking outputs that slept.
        std::uint64_t sas_mixes = 0;
        std::uint64_t atrac_decodes = 0;
        std::uint32_t channels_reserved = 0; // ChReserve/Output2/SRC that succeeded.
    };
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }

    // AudioWaitHost.
    bool IsWaitingOnAudio(std::uint64_t thread) override;
    void WakeFromAudio(std::uint64_t thread, std::uint32_t result) override;

private:
    void start_mixer();
    void schedule_update();
    void update();
    // A blocking output's tail: sleeps when the channel had samples queued.
    std::uint32_t finish_blocking(std::uint32_t channel, std::uint32_t result, bool must_wait);
    [[nodiscard]] audio::AudioChannels::Caller caller() const;

    System &system_;
    kernel::Kernel &kernel_;
    audio::GuestMemory memory_;
    audio::AudioChannels channels_;
    audio::SasCore sas_;
    audio::AtracManager atrac_;
    Output output_;
    bool mixer_started_ = false;
    std::uint64_t next_update_ = 0; // Index of the next update since boot.
    std::array<std::int32_t, audio::kHwBlockSize * 2> mix_{};
    Stats stats_;
};

} // namespace mpo::hle
