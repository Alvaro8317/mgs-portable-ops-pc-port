// SPDX-License-Identifier: GPL-3.0-or-later
//
// Switches for the PPSSPP reference build (see glue.cpp).

#pragma once

// When true, PPSSPP's AtracBase::CreateDecoder gets the fake decoder of
// audio_scenarios.h instead of the real ATRAC codec.
extern bool g_ppssppRefFakeDecoder;
