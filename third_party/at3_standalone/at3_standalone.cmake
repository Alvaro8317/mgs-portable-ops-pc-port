# SPDX-License-Identifier: GPL-3.0-or-later
#
# at3_standalone: the ATRAC3/ATRAC3plus decoder that PPSSPP extracted from
# FFmpeg (LGPL-2.1-or-later). The sources in src/ are verbatim copies of
# PPSSPP v1.20.4 ext/at3_standalone; shim/ replaces the few PPSSPP headers they
# include. See NOTICE.md.

if(TARGET mpo_at3_standalone)
    return()
endif()

set(mpo_at3_dir "${CMAKE_CURRENT_LIST_DIR}")
add_library(mpo_at3_standalone STATIC
    ${mpo_at3_dir}/src/atrac.cpp
    ${mpo_at3_dir}/src/atrac3.cpp
    ${mpo_at3_dir}/src/atrac3plus.cpp
    ${mpo_at3_dir}/src/atrac3plusdec.cpp
    ${mpo_at3_dir}/src/atrac3plusdsp.cpp
    ${mpo_at3_dir}/src/get_bits.cpp
    ${mpo_at3_dir}/src/compat.cpp
    ${mpo_at3_dir}/src/fft.cpp
    ${mpo_at3_dir}/src/mem.cpp
    ${mpo_at3_dir}/shim/shim.cpp
)
target_include_directories(mpo_at3_standalone PRIVATE ${mpo_at3_dir}/shim)
# Consumers include "at3_decoders.h" only.
target_include_directories(mpo_at3_standalone SYSTEM INTERFACE ${mpo_at3_dir}/src)
set_target_properties(mpo_at3_standalone PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(mpo_at3_standalone PRIVATE /W0)
    target_compile_definitions(mpo_at3_standalone PRIVATE _USE_MATH_DEFINES)
else()
    # Third-party code: keep it quiet and out of MPO_WARNINGS_AS_ERRORS.
    target_compile_options(mpo_at3_standalone PRIVATE -w)
endif()
