// SPDX-License-Identifier: GPL-3.0-or-later
//
// Counters of what the GE executed and, above all, of what it could not do.
// Nothing unsupported is dropped silently (CLAUDE.md §5): every such case is
// counted under a name and reported once through the log callback.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace mpo::ge {

struct Stats {
    std::uint64_t commands = 0;
    std::uint64_t draw_commands = 0; // PRIM, BEZIER, SPLINE
    std::uint64_t vertices = 0;
    std::uint64_t triangles = 0;
    std::uint64_t rectangles = 0;
    std::uint64_t sprites = 0;
    std::uint64_t clear_rects = 0;
    std::uint64_t points = 0;
    std::uint64_t lines = 0;
    std::uint64_t culled = 0;
    // Triangles that crossed the -Z plane and went through the clipper.
    std::uint64_t clipped_triangles = 0;
    std::uint64_t patches = 0; // BEZIER and SPLINE tessellated
    std::uint64_t bbox_tests = 0;
    std::uint64_t bbox_culled = 0;
    std::uint64_t clut_loads = 0;
    std::uint64_t block_transfers = 0;
    // Feature name -> occurrences (draws or commands) that were not executed
    // or not executed exactly.
    std::map<std::string, std::uint64_t, std::less<>> unsupported;

    // Optional sink for one line per new unsupported feature.
    std::function<void(std::string_view)> log;

    void Unsupported(std::string_view what) {
        auto it = unsupported.find(what);
        if (it == unsupported.end()) {
            unsupported.emplace(std::string(what), 1);
            if (log) log(what);
        } else {
            ++it->second;
        }
    }
};

} // namespace mpo::ge
