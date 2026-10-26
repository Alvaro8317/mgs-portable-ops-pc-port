# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_font (F17a): PGF reader and glyph rasterizer, ported from PPSSPP v1.20.4
# Core/Font/PGF.cpp. No HLE registration here (sceFont is F17b).

add_library(mpo_font STATIC pgf.cpp)
target_include_directories(mpo_font PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mpo_target_warnings(mpo_font)

# Local analysis tool: renders a glyph sheet of a .pgf to PNG.
add_executable(mpo_pgf_dump pgf_dump.cpp)
target_link_libraries(mpo_pgf_dump PRIVATE mpo_font mpo_ge_replay)
mpo_target_warnings(mpo_pgf_dump)
