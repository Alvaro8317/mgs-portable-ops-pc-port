// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for PPSSPP's savestate serializer: the difftest never saves state,
// so every Do*() is a no-op and every section reports version 0.

#pragma once

#include <cstddef>

class PointerWrap {
public:
    enum Mode { MODE_READ = 1, MODE_WRITE, MODE_MEASURE, MODE_VERIFY, MODE_NOOP };
    Mode mode = MODE_NOOP;
    int Section(const char *, int, int = 0) { return 0; }
    void DoMarker(const char *, int = 0) {}
};

template <typename... Args>
void Do(PointerWrap &, Args &&...) {}
template <typename... Args>
void DoArray(PointerWrap &, Args &&...) {}
template <typename... Args>
void DoClass(PointerWrap &, Args &&...) {}
