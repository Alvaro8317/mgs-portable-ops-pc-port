# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_recomp (F07b): what the recompiled corpus needs from the profile at run
# time. Today, mpo_vfpu as the PSPRecomp VFPU executor. It only sees the
# framework headers: the executable that links it also links psprecomp_core
# (an object library), so the runtime objects are linked exactly once.

add_library(mpo_recomp STATIC vfpu_executor.cpp)
target_include_directories(mpo_recomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(mpo_recomp SYSTEM PUBLIC
    $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(mpo_recomp PUBLIC mpo_vfpu)
mpo_target_warnings(mpo_recomp)

# The recompiled mgp_main, from the user's own executable (never versioned).
# Generate it with profiles/mpo/scripts/generate.py and point this at its
# <out>/generated directory. Empty: nothing below is built (CI never has it).
set(MPO_GENERATED_DIR "" CACHE PATH
    "psp_recomp output for mgp_main (generate.py <out>/generated); empty: not built")
if(MPO_GENERATED_DIR)
    if(NOT EXISTS "${MPO_GENERATED_DIR}/generated_registry.cpp")
        message(FATAL_ERROR "MPO_GENERATED_DIR has no generated_registry.cpp: ${MPO_GENERATED_DIR}")
    endif()
    file(GLOB mpo_generated_sources CONFIGURE_DEPENDS "${MPO_GENERATED_DIR}/generated_*.cpp")
    list(LENGTH mpo_generated_sources mpo_generated_count)
    message(STATUS "mgp_main corpus: ${mpo_generated_count} files in ${MPO_GENERATED_DIR}")
    add_library(mpo_generated_mgp_main OBJECT ${mpo_generated_sources})
    target_include_directories(mpo_generated_mgp_main PRIVATE "${MPO_GENERATED_DIR}")
    target_include_directories(mpo_generated_mgp_main SYSTEM PRIVATE
        $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
    # Each unit is a single huge function: compile them in the framework's
    # memory-bounded pool (PSPRECOMP_GENERATED_JOBS, Ninja only), optimised
    # whatever the build type, without warnings, sanitizers or float
    # contraction (the VFPU goes through mpo_vfpu, the FPU must stay exact).
    set_target_properties(mpo_generated_mgp_main PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
    if(MSVC)
        target_compile_options(mpo_generated_mgp_main PRIVATE /W0 /bigobj)
    else()
        target_compile_options(mpo_generated_mgp_main PRIVATE -w -ffp-contract=off -fno-sanitize=all
                               -O${PSPRECOMP_GENERATED_OPT_LEVEL})
    endif()

    add_executable(mpo_aot_probe aot_probe.cpp)
    target_link_libraries(mpo_aot_probe PRIVATE psprecomp_core mpo_recomp mpo_generated_mgp_main)
    mpo_target_warnings(mpo_aot_probe)
endif()
