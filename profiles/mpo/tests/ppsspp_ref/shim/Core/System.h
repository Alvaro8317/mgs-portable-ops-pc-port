// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only stand-in for PPSSPP Core/System.h (v1.20.4). It declares only what the
// interpreter sources compiled by mpo_ppsspp_ref use; the definitions live in
// profiles/mpo/tests/ppsspp_ref/ppsspp_ref.cpp.

#pragma once

// PSP_CoreParameter().compat.flags().MoreAccurateVMMUL is the only use.
struct MpoCompatFlagsShim {
    bool MoreAccurateVMMUL = false;
};

struct MpoCompatShim {
    MpoCompatFlagsShim flags_;
    const MpoCompatFlagsShim &flags() const { return flags_; }
};

struct MpoCoreParameterShim {
    MpoCompatShim compat;
};

MpoCoreParameterShim &PSP_CoreParameter();
