// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/SasReverb.h @ v1.20.4
// Copyright (c) 2015- PPSSPP Project (GPL-2.0-or-later).
//
// SAS reverb, modelled on the PSX SPU reverb.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpo::audio {

class SasReverb {
public:
    SasReverb();

    void SetPreset(int preset);
    int GetPreset() const { return preset_; }

    static const char *GetPresetName(int preset);

    // Input is a mixdown of the voices with effects enabled, at 22 kHz.
    // Output is written back at 44 kHz (4 values per input frame).
    // `volume_multiplier` is PPSSPP's reverb volume setting (1.0 by default).
    void ProcessReverb(int16_t *output, const int16_t *input, size_t inputSize, int volLeft,
                       int volRight, float volume_multiplier = 1.0f);

private:
    static constexpr int kBufSize = 0x20000;

    std::vector<int16_t> workspace_;
    int preset_ = -1;
    int pos_ = 0;
};

} // namespace mpo::audio
