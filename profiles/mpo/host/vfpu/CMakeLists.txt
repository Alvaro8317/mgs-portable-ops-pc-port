# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_vfpu: VFPU state and semantics ported bit-exact from PPSSPP v1.20.4
# (GPL-2.0-or-later code, so it lives in the GPL profile, never in the MIT
# framework). Game-independent; the tables are loaded at run time.

include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_cpu/ppsspp_vfpu_tables.cmake)

add_library(mpo_vfpu STATIC
    vfpu_math.cpp
    vfpu_ops.cpp
)
# Headers are included as "vfpu/vfpu.hpp".
target_include_directories(mpo_vfpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(NOT MSVC)
    # Bit-exactness: never fuse a*b+c, whatever -march says. MSVC does not
    # contract by default (/fp:precise).
    target_compile_options(mpo_vfpu PRIVATE -ffp-contract=off)
endif()
mpo_target_warnings(mpo_vfpu)
