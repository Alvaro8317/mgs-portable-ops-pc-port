// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceAudio, sceSasCore and sceAtrac3plus (F16b). The NID tables, argument
// order, cycles eaten and thread delays are ported from PPSSPP
// Core/HLE/sceAudio.cpp, __sceAudio.cpp, sceSas.cpp and sceAtrac.cpp
// @ v1.20.4; the work itself is mpo_audio's (F16a).

#include "hle/audio.hpp"

#include "hle/hle_args.hpp"
#include "hle/system.hpp"

namespace mpo::hle {

namespace {

// __AudioCPUMHzChange: usToCycles(1000000) * hwBlockSize / hwSampleRate,
// truncated to whole cycles.
constexpr std::uint64_t kUpdateCycles =
    kCpuHz * static_cast<std::uint64_t>(audio::kHwBlockSize) / audio::kHwSampleRate;

// What PPSSPP's NotifyLoadStatusAtrac gives the module: the latest version.
constexpr int kAtracLibVersion = 0x105;
constexpr std::uint32_t kAtracBssSize = 0x67Cu;

std::int32_t sarg(const Ctx &ctx, int index) {
    return static_cast<std::int32_t>(arg(ctx, index));
}

} // namespace

Audio::Audio(System &system)
    : system_(system), kernel_(system.kernel()),
      // The audio code only sees RAM: sample buffers, SAS cores and the
      // atrac contexts all live there.
      memory_(const_cast<std::uint8_t *>(system.runtime().memory().bytes().data()),
              psprecomp::GuestMemory::kPhysicalBase,
              static_cast<std::uint32_t>(system.runtime().memory().bytes().size())),
      channels_(memory_), atrac_(memory_) {
    channels_.SetWaitHost(this);
    sas_.SetAtracBridge(&atrac_);
}

void Audio::notify_atrac_module(bool loaded, std::uint32_t address) {
    if (!loaded) {
        atrac_.NotifyUnloadModule();
        return;
    }
    const bool ok = atrac_.NotifyLoadModule(kAtracLibVersion, address, kAtracBssSize);
    system_.hle().trace_event("atrac: libatrac3plus %X, %d contexts at %08X%s", kAtracLibVersion,
                              atrac_.MaxContexts(), address, ok ? "" : " (bad BSS)");
}

bool Audio::IsWaitingOnAudio(std::uint64_t thread) {
    kernel::ThreadInfo info;
    return kernel_.thread_info(static_cast<kernel::Uid>(thread), info) &&
           (info.status & kernel::thread_status::kWait) != 0u &&
           info.wait_type == kernel::WaitType::kAudioChannel;
}

void Audio::WakeFromAudio(std::uint64_t thread, std::uint32_t result) {
    kernel_.wake_from_wait(static_cast<kernel::Uid>(thread), result);
}

audio::AudioChannels::Caller Audio::caller() const {
    return {static_cast<std::uint64_t>(kernel_.current_thread()), kernel_.dispatch_enabled()};
}

std::uint32_t Audio::finish_blocking(std::uint32_t channel, std::uint32_t result, bool must_wait) {
    if (!must_wait) return result;
    ++stats_.blocking_waits;
    // __KernelWaitCurThread(WAITTYPE_AUDIOCHANNEL, chanNum + 1, ret, 0, false).
    return kernel_.wait_current_thread(kernel::WaitType::kAudioChannel,
                                       static_cast<kernel::Uid>(channel + 1u), 0u, 0u, false,
                                       result);
}

// ---- The hardware mixer --------------------------------------------------------

void Audio::start_mixer() {
    if (mixer_started_) return;
    mixer_started_ = true;
    // PPSSPP schedules the first update at boot: keep its phase.
    const std::uint64_t now_cycles = kernel_.now_us() * kCpuHz / 1'000'000u;
    next_update_ = now_cycles / kUpdateCycles + 1u;
    schedule_update();
}

void Audio::schedule_update() {
    // The first microsecond at or after the update's cycle, on an exact timeline.
    const std::uint64_t cycles = next_update_ * kUpdateCycles;
    const std::uint64_t target_us = (cycles * 1'000'000u + kCpuHz - 1u) / kCpuHz;
    const std::uint64_t now = kernel_.now_us();
    (void)kernel_.schedule_event(target_us > now ? target_us - now : 0u, [this] { update(); });
}

void Audio::update() {
    if (stats_.blocks == 0u) stats_.first_block_us = kernel_.now_us();
    ++stats_.blocks;
    channels_.Update(mix_.data());
    for (const std::int32_t sample : mix_) {
        if (sample != 0) {
            ++stats_.audible_blocks;
            break;
        }
    }
    if (output_) output_(mix_.data(), audio::kHwBlockSize);
    ++next_update_;
    schedule_update();
}

// ---- NIDs ------------------------------------------------------------------------

void Audio::register_hle() {
    HleTable &t = system_.hle();
    kernel::Kernel &k = kernel_;
    GameClock &clock = system_.clock();
    audio::AudioChannels &ch = channels_;
    const auto def = [&t](const char *library, std::uint32_t nid, const char *name,
                          HleTable::Handler handler) {
        t.add(library, nid, name, HleKind::kImplemented, std::move(handler));
    };
    // A reservation starts the mixer the first time it succeeds.
    const auto reserved = [this](std::uint32_t result) {
        if (static_cast<std::int32_t>(result) >= 0) {
            ++stats_.channels_reserved;
            start_mixer();
        }
        return result;
    };

    // Output2 and SRC eat 10000 cycles once the volume and the channel pass
    // their checks, before enqueueing (the enqueue itself can only fail with
    // CAN_NOT_WAIT).
    const auto eat = [&clock](std::uint32_t result) {
        if (static_cast<std::int32_t>(result) >= 0 || result == kernel::error::kCanNotWait)
            clock.eat_cycles(10000u);
    };

    // ---- sceAudio ----
    const char *const kAudio = "sceAudio";
    def(kAudio, 0x01562BA3u, "sceAudioOutput2Reserve",
        [&ch, reserved](Ctx &c) { ret(c, reserved(ch.Output2Reserve(arg(c, 0)))); });
    def(kAudio, 0x2D53F36Eu, "sceAudioOutput2OutputBlocking", [this, &ch, eat](Ctx &c) {
        bool must_wait = false;
        const std::uint32_t result =
            ch.Output2OutputBlocking(arg(c, 0), arg(c, 1), caller(), &must_wait);
        eat(result);
        ret(c, finish_blocking(audio::PSP_AUDIO_CHANNEL_OUTPUT2, result, must_wait));
    });
    def(kAudio, 0x63F2889Cu, "sceAudioOutput2ChangeLength",
        [&ch](Ctx &c) { ret(c, ch.Output2ChangeLength(arg(c, 0))); });
    def(kAudio, 0x647CEF33u, "sceAudioOutput2GetRestSample",
        [&ch](Ctx &c) { ret(c, ch.Output2GetRestSample()); });
    def(kAudio, 0x43196845u, "sceAudioOutput2Release",
        [&ch](Ctx &c) { ret(c, ch.Output2Release()); });
    def(kAudio, 0x80F1F7E0u, "sceAudioInit", [](Ctx &c) { ret(c, 0u); });
    def(kAudio, 0x210567F7u, "sceAudioEnd", [](Ctx &c) { ret(c, 0u); });
    def(kAudio, 0xA2BEAA6Cu, "sceAudioSetFrequency",
        [&ch](Ctx &c) { ret(c, ch.SetFrequency(arg(c, 0))); });
    def(kAudio, 0x927AC32Bu, "sceAudioSetVolumeOffset", [](Ctx &c) { ret(c, 0u); });
    def(kAudio, 0x8C1009B2u, "sceAudioOutput",
        [&ch](Ctx &c) { ret(c, ch.Output(arg(c, 0), sarg(c, 1), arg(c, 2))); });
    def(kAudio, 0x136CAF51u, "sceAudioOutputBlocking", [this, &ch](Ctx &c) {
        bool must_wait = false;
        const std::uint32_t result =
            ch.OutputBlocking(arg(c, 0), sarg(c, 1), arg(c, 2), caller(), &must_wait);
        ret(c, finish_blocking(arg(c, 0), result, must_wait));
    });
    def(kAudio, 0xE2D56B2Du, "sceAudioOutputPanned",
        [&ch](Ctx &c) { ret(c, ch.OutputPanned(arg(c, 0), sarg(c, 1), sarg(c, 2), arg(c, 3))); });
    def(kAudio, 0x13F592BCu, "sceAudioOutputPannedBlocking", [this, &ch](Ctx &c) {
        bool must_wait = false;
        const std::uint32_t result = ch.OutputPannedBlocking(arg(c, 0), sarg(c, 1), sarg(c, 2),
                                                             arg(c, 3), caller(), &must_wait);
        ret(c, finish_blocking(arg(c, 0), result, must_wait));
    });
    def(kAudio, 0x5EC81C55u, "sceAudioChReserve", [this, &ch, reserved](Ctx &c) {
        const std::uint32_t result = reserved(ch.ChReserve(sarg(c, 0), arg(c, 1), arg(c, 2)));
        system_.hle().trace_event("audio: reserve channel %d, %u samples, format %X -> %08X",
                                  sarg(c, 0), arg(c, 1), arg(c, 2), result);
        ret(c, result);
    });
    def(kAudio, 0x6FC46853u, "sceAudioChRelease",
        [&ch](Ctx &c) { ret(c, ch.ChRelease(arg(c, 0))); });
    def(kAudio, 0xE9D97901u, "sceAudioGetChannelRestLen",
        [&ch](Ctx &c) { ret(c, static_cast<std::uint32_t>(ch.GetChannelRestLen(arg(c, 0)))); });
    def(kAudio, 0xB011922Fu, "sceAudioGetChannelRestLength",
        [&ch](Ctx &c) { ret(c, static_cast<std::uint32_t>(ch.GetChannelRestLength(arg(c, 0)))); });
    def(kAudio, 0xCB2E439Eu, "sceAudioSetChannelDataLen",
        [&ch](Ctx &c) { ret(c, ch.SetChannelDataLen(arg(c, 0), arg(c, 1))); });
    def(kAudio, 0x95FD0C2Du, "sceAudioChangeChannelConfig",
        [&ch](Ctx &c) { ret(c, ch.ChangeChannelConfig(arg(c, 0), arg(c, 1))); });
    def(kAudio, 0xB7E1D8E7u, "sceAudioChangeChannelVolume",
        [&ch](Ctx &c) { ret(c, ch.ChangeChannelVolume(arg(c, 0), arg(c, 1), arg(c, 2))); });
    def(kAudio, 0x38553111u, "sceAudioSRCChReserve", [&ch, reserved](Ctx &c) {
        ret(c, reserved(ch.SRCChReserve(arg(c, 0), arg(c, 1), arg(c, 2))));
    });
    def(kAudio, 0x5C37C0AEu, "sceAudioSRCChRelease", [&ch](Ctx &c) { ret(c, ch.SRCChRelease()); });
    def(kAudio, 0xE0727056u, "sceAudioSRCOutputBlocking", [this, &ch, eat](Ctx &c) {
        bool must_wait = false;
        const std::uint32_t result =
            ch.SRCOutputBlocking(arg(c, 0), arg(c, 1), caller(), &must_wait);
        eat(result);
        ret(c, finish_blocking(audio::PSP_AUDIO_CHANNEL_SRC, result, must_wait));
    });

    // ---- sceSasCore ----
    const char *const kSas = "sceSasCore";
    audio::SasCore &sas = sas_;
    const audio::GuestMemory &m = memory_;
    // delaySasResult: only a mix that ran delays the thread.
    const auto mixed = [this, &k, &sas](std::uint32_t result) {
        if (result != 0u) return result;
        ++stats_.sas_mixes;
        return k.delay_result(0u, static_cast<std::uint32_t>(sas.EstimateMixUs()));
    };
    def(kSas, 0x42778A9Fu, "__sceSasInit", [this, &sas, &m](Ctx &c) {
        const std::uint32_t result =
            sas.Init(m, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4));
        system_.hle().trace_event("sas: init core %08X grain %u voices %u mode %u rate %u -> %08X",
                                  arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), result);
        ret(c, result);
    });
    def(kSas, 0xA3589D81u, "__sceSasCore", [&k, &sas, &m, mixed](Ctx &c) {
        ret(c, mixed(sas.Core(m, arg(c, 0), arg(c, 1), k.dispatch_enabled())));
    });
    def(kSas, 0x50A14DFCu, "__sceSasCoreWithMix", [&k, &sas, &m, mixed](Ctx &c) {
        ret(c, mixed(sas.CoreWithMix(m, arg(c, 0), arg(c, 1), sarg(c, 2), sarg(c, 3),
                                     k.dispatch_enabled())));
    });
    def(kSas, 0x68A46B95u, "__sceSasGetEndFlag",
        [&sas](Ctx &c) { ret(c, sas.GetEndFlag(arg(c, 0))); });
    def(kSas, 0x440CA7D8u, "__sceSasSetVolume", [&sas](Ctx &c) {
        ret(c,
            sas.SetVolume(arg(c, 0), sarg(c, 1), sarg(c, 2), sarg(c, 3), sarg(c, 4), sarg(c, 5)));
    });
    def(kSas, 0xAD84D37Fu, "__sceSasSetPitch",
        [&sas](Ctx &c) { ret(c, sas.SetPitch(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0x99944089u, "__sceSasSetVoice", [&sas, &m](Ctx &c) {
        ret(c, sas.SetVoice(m, arg(c, 0), sarg(c, 1), arg(c, 2), sarg(c, 3), sarg(c, 4)));
    });
    def(kSas, 0xB7660A23u, "__sceSasSetNoise",
        [&sas](Ctx &c) { ret(c, sas.SetNoise(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0x019B25EBu, "__sceSasSetADSR", [&sas](Ctx &c) {
        ret(c, sas.SetADSR(arg(c, 0), sarg(c, 1), sarg(c, 2), sarg(c, 3), sarg(c, 4), sarg(c, 5),
                           sarg(c, 6)));
    });
    def(kSas, 0x9EC3676Au, "__sceSasSetADSRmode", [&sas](Ctx &c) {
        ret(c, sas.SetADSRMode(arg(c, 0), sarg(c, 1), sarg(c, 2), sarg(c, 3), sarg(c, 4),
                               sarg(c, 5), sarg(c, 6)));
    });
    def(kSas, 0x5F9529F6u, "__sceSasSetSL",
        [&sas](Ctx &c) { ret(c, sas.SetSL(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0x74AE582Au, "__sceSasGetEnvelopeHeight",
        [&sas](Ctx &c) { ret(c, sas.GetEnvelopeHeight(arg(c, 0), sarg(c, 1))); });
    def(kSas, 0xCBCD4F79u, "__sceSasSetSimpleADSR",
        [&sas](Ctx &c) { ret(c, sas.SetSimpleADSR(arg(c, 0), sarg(c, 1), arg(c, 2), arg(c, 3))); });
    def(kSas, 0xA0CF2FA4u, "__sceSasSetKeyOff",
        [&sas](Ctx &c) { ret(c, sas.SetKeyOff(arg(c, 0), sarg(c, 1))); });
    def(kSas, 0x76F01ACAu, "__sceSasSetKeyOn",
        [&sas, &m](Ctx &c) { ret(c, sas.SetKeyOn(m, arg(c, 0), sarg(c, 1))); });
    def(kSas, 0xF983B186u, "__sceSasRevVON",
        [&sas](Ctx &c) { ret(c, sas.RevVON(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0xD5A229C9u, "__sceSasRevEVOL",
        [&sas](Ctx &c) { ret(c, sas.RevEVOL(arg(c, 0), arg(c, 1), arg(c, 2))); });
    def(kSas, 0x33D4AB37u, "__sceSasRevType",
        [&sas](Ctx &c) { ret(c, sas.RevType(arg(c, 0), sarg(c, 1))); });
    def(kSas, 0x267A6DD2u, "__sceSasRevParam",
        [&sas](Ctx &c) { ret(c, sas.RevParam(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0x2C8E6AB3u, "__sceSasGetPauseFlag",
        [&sas](Ctx &c) { ret(c, sas.GetPauseFlag(arg(c, 0))); });
    def(kSas, 0x787D04D5u, "__sceSasSetPause",
        [&sas](Ctx &c) { ret(c, sas.SetPause(arg(c, 0), arg(c, 1), sarg(c, 2))); });
    def(kSas, 0xA232CBE6u, "__sceSasSetTrianglarWave",
        [&sas](Ctx &c) { ret(c, sas.SetTriangularWave(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0xD5EBBBCDu, "__sceSasSetSteepWave",
        [&sas](Ctx &c) { ret(c, sas.SetSteepWave(arg(c, 0), sarg(c, 1), sarg(c, 2))); });
    def(kSas, 0xBD11B7C2u, "__sceSasGetGrain", [&sas](Ctx &c) { ret(c, sas.GetGrain(arg(c, 0))); });
    def(kSas, 0xD1E0A01Eu, "__sceSasSetGrain",
        [&sas](Ctx &c) { ret(c, sas.SetGrain(arg(c, 0), sarg(c, 1))); });
    def(kSas, 0xE175EF66u, "__sceSasGetOutputmode",
        [&sas](Ctx &c) { ret(c, sas.GetOutputMode(arg(c, 0))); });
    def(kSas, 0xE855BF76u, "__sceSasSetOutputmode",
        [&sas](Ctx &c) { ret(c, sas.SetOutputMode(arg(c, 0), arg(c, 1))); });
    def(kSas, 0x07F58C24u, "__sceSasGetAllEnvelopeHeights",
        [&sas, &m](Ctx &c) { ret(c, sas.GetAllEnvelopeHeights(m, arg(c, 0), arg(c, 1))); });
    def(kSas, 0xE1CD9561u, "__sceSasSetVoicePCM", [&sas, &m](Ctx &c) {
        ret(c, sas.SetVoicePCM(m, arg(c, 0), sarg(c, 1), arg(c, 2), sarg(c, 3), sarg(c, 4)));
    });
    def(kSas, 0x4AA9EAD6u, "__sceSasSetVoiceATRAC3",
        [&sas, &m](Ctx &c) { ret(c, sas.SetVoiceATRAC3(m, arg(c, 0), sarg(c, 1), arg(c, 2))); });
    def(kSas, 0x7497EA85u, "__sceSasConcatenateATRAC3", [&sas](Ctx &c) {
        ret(c, sas.ConcatenateATRAC3(arg(c, 0), sarg(c, 1), arg(c, 2), sarg(c, 3)));
    });
    def(kSas, 0xF6107F00u, "__sceSasUnsetATRAC3",
        [&sas, &m](Ctx &c) { ret(c, sas.UnsetATRAC3(m, arg(c, 0), sarg(c, 1))); });

    // ---- sceAtrac3plus (PPSSPP registers the table under both names) ----
    audio::AtracManager &at = atrac_;
    // hleDelayResult when PPSSPP delays the call.
    const auto delayed = [&k, &at](std::uint32_t result) {
        const int usec = at.LastDelayUs();
        return usec > 0 ? k.delay_result(result, static_cast<std::uint32_t>(usec)) : result;
    };
    const auto u = [](int value) { return static_cast<std::uint32_t>(value); };
    for (const char *library : {"sceAtrac3plus", "sceATRAC3plus_Library"}) {
        const auto atr = [&def, library](std::uint32_t nid, const char *name,
                                         HleTable::Handler handler) {
            def(library, nid, name, std::move(handler));
        };
        atr(0x7DB31251u, "sceAtracAddStreamData",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.AddStreamData(sarg(c, 0), arg(c, 1)))); });
        atr(0x6A8C3CD5u, "sceAtracDecodeData", [this, &at, delayed](Ctx &c) {
            ++stats_.atrac_decodes;
            ret(c, delayed(at.DecodeData(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4))));
        });
        atr(0xD5C28CC0u, "sceAtracReleaseResources",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.ReleaseResources())); });
        atr(0x780F88D1u, "sceAtracGetAtracID",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetAtracID(sarg(c, 0)))); });
        atr(0xCA3CA3D2u, "sceAtracGetBufferInfoForReseting", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetBufferInfoForResetting(sarg(c, 0), sarg(c, 1), arg(c, 2))));
        });
        atr(0x2DD3E298u, "sceAtracGetBufferInfoForResetting", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetBufferInfoForResetting(sarg(c, 0), sarg(c, 1), arg(c, 2))));
        });
        atr(0xA554A158u, "sceAtracGetBitrate",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetBitrate(sarg(c, 0), arg(c, 1)))); });
        atr(0x31668BAAu, "sceAtracGetChannel",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetChannel(sarg(c, 0), arg(c, 1)))); });
        atr(0xFAA4F89Bu, "sceAtracGetLoopStatus", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetLoopStatus(sarg(c, 0), arg(c, 1), arg(c, 2))));
        });
        atr(0xE88F759Bu, "sceAtracGetInternalErrorInfo", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetInternalErrorInfo(sarg(c, 0), arg(c, 1))));
        });
        atr(0xD6A5F2F7u, "sceAtracGetMaxSample",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetMaxSample(sarg(c, 0), arg(c, 1)))); });
        atr(0xE23E3A35u, "sceAtracGetNextDecodePosition", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetNextDecodePosition(sarg(c, 0), arg(c, 1))));
        });
        atr(0x36FAABFBu, "sceAtracGetNextSample",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetNextSample(sarg(c, 0), arg(c, 1)))); });
        atr(0x9AE849A7u, "sceAtracGetRemainFrame",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetRemainFrame(sarg(c, 0), arg(c, 1)))); });
        atr(0x83E85EA0u, "sceAtracGetSecondBufferInfo", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetSecondBufferInfo(sarg(c, 0), arg(c, 1), arg(c, 2))));
        });
        atr(0xA2BBA8BEu, "sceAtracGetSoundSample", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetSoundSample(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))));
        });
        atr(0x5D268707u, "sceAtracGetStreamDataInfo", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.GetStreamDataInfo(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))));
        });
        atr(0x61EB33F5u, "sceAtracReleaseAtracID",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.ReleaseAtracID(sarg(c, 0)))); });
        atr(0x644E5607u, "sceAtracResetPlayPosition", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.ResetPlayPosition(sarg(c, 0), sarg(c, 1), sarg(c, 2), sarg(c, 3))));
        });
        atr(0x3F6E26B5u, "sceAtracSetHalfwayBuffer", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.SetHalfwayBuffer(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))));
        });
        atr(0x83BF7AFDu, "sceAtracSetSecondBuffer", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.SetSecondBuffer(sarg(c, 0), arg(c, 1), arg(c, 2))));
        });
        atr(0x0E2A73ABu, "sceAtracSetData", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.SetData(sarg(c, 0), arg(c, 1), arg(c, 2))));
        });
        atr(0x7A20E7AFu, "sceAtracSetDataAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetDataAndGetID(arg(c, 0), sarg(c, 1)))));
        });
        atr(0xD1F59FDBu, "sceAtracStartEntry",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.StartEntry())); });
        atr(0x868120B5u, "sceAtracSetLoopNum",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.SetLoopNum(sarg(c, 0), sarg(c, 1)))); });
        atr(0x132F1ECAu, "sceAtracReinit",
            [&at, delayed, u](Ctx &c) { ret(c, delayed(u(at.Reinit(sarg(c, 0), sarg(c, 1))))); });
        atr(0xECA32A99u, "sceAtracIsSecondBufferNeeded",
            [&at, delayed, u](Ctx &c) { ret(c, delayed(u(at.IsSecondBufferNeeded(sarg(c, 0))))); });
        atr(0x0FAE370Eu, "sceAtracSetHalfwayBufferAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetHalfwayBufferAndGetID(arg(c, 0), arg(c, 1), arg(c, 2)))));
        });
        atr(0x5CF9D852u, "sceAtracSetMOutHalfwayBuffer", [&at, delayed, u](Ctx &c) {
            ret(c,
                delayed(u(at.SetMOutHalfwayBuffer(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)))));
        });
        atr(0xF6837A1Au, "sceAtracSetMOutData", [&at, delayed](Ctx &c) {
            ret(c, delayed(at.SetMOutData(sarg(c, 0), arg(c, 1), arg(c, 2))));
        });
        atr(0x472E3825u, "sceAtracSetMOutDataAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetMOutDataAndGetID(arg(c, 0), arg(c, 1)))));
        });
        atr(0x9CD7DE03u, "sceAtracSetMOutHalfwayBufferAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetMOutHalfwayBufferAndGetID(arg(c, 0), arg(c, 1), arg(c, 2)))));
        });
        atr(0xB3B5D042u, "sceAtracGetOutputChannel", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.GetOutputChannel(sarg(c, 0), arg(c, 1)))));
        });
        atr(0x5622B7C1u, "sceAtracSetAA3DataAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetAA3DataAndGetID(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)))));
        });
        atr(0x5DD66588u, "sceAtracSetAA3HalfwayBufferAndGetID", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.SetAA3HalfwayBufferAndGetID(arg(c, 0), arg(c, 1), arg(c, 2),
                                                            arg(c, 3)))));
        });
        atr(0x231FC6B7u, "_sceAtracGetContextAddress",
            [&at, delayed](Ctx &c) { ret(c, delayed(at.GetContextAddress(sarg(c, 0)))); });
        atr(0x1575D64Bu, "sceAtracLowLevelInitDecoder", [&at, delayed, u](Ctx &c) {
            ret(c, delayed(u(at.LowLevelInitDecoder(sarg(c, 0), arg(c, 1)))));
        });
        atr(0x0C116E1Bu, "sceAtracLowLevelDecode", [this, &at, delayed, u](Ctx &c) {
            ++stats_.atrac_decodes;
            ret(c, delayed(u(
                       at.LowLevelDecode(sarg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)))));
        });
    }
}

} // namespace mpo::hle
