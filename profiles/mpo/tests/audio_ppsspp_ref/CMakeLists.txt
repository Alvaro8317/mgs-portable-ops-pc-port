# SPDX-License-Identifier: GPL-3.0-or-later
#
# Optional difftest of mpo_audio against PPSSPP v1.20.4's own audio code,
# compiled from the checkout in MPO_PPSSPP_SOURCE_DIR (never vendored here).
# shim/ replaces the PPSSPP headers those files would drag in (logging,
# savestates, memory map, config).

set(mpo_ppsspp "${MPO_PPSSPP_SOURCE_DIR}")
set(mpo_ppsspp_ref_sources
    ${mpo_ppsspp}/Core/HW/SasAudio.cpp
    ${mpo_ppsspp}/Core/HW/SasReverb.cpp
    ${mpo_ppsspp}/Core/HLE/AtracCtx2.cpp
    ${mpo_ppsspp}/Core/Util/AtracTrack.cpp
    ${mpo_ppsspp}/Core/HW/Atrac3Standalone.cpp
)
foreach(mpo_src IN LISTS mpo_ppsspp_ref_sources)
    if(NOT EXISTS "${mpo_src}")
        message(FATAL_ERROR "MPO_AUDIO_PPSSPP_DIFFTEST: missing ${mpo_src}")
    endif()
endforeach()

add_library(mpo_audio_ppsspp_ref STATIC ${mpo_ppsspp_ref_sources} glue.cpp)
# The shims must win over the real headers, so they come first.
target_include_directories(mpo_audio_ppsspp_ref BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${mpo_ppsspp}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mpo_audio_ppsspp_ref PUBLIC mpo_audio mpo_at3_standalone)
# Upstream code: no project warnings, and the two codec factories are renamed
# so glue.cpp can put the fake decoder in front of them.
if(MSVC)
    target_compile_options(mpo_audio_ppsspp_ref PRIVATE /W0)
else()
    # Upstream relies on wrapping int overflow (SasReverb's comb filter), which
    # UBSan rejects; mpo_audio computes the same values without UB.
    target_compile_options(mpo_audio_ppsspp_ref PRIVATE -w -fno-sanitize=undefined)
endif()
set_source_files_properties(${mpo_ppsspp}/Core/HW/Atrac3Standalone.cpp PROPERTIES
    COMPILE_DEFINITIONS
    "CreateAtrac3Audio=Upstream_CreateAtrac3Audio;CreateAtrac3PlusAudio=Upstream_CreateAtrac3PlusAudio")

add_executable(mpo_audio_ppsspp_difftest audio_ppsspp_difftest.cpp)
target_link_libraries(mpo_audio_ppsspp_difftest PRIVATE mpo_audio_ppsspp_ref mpo_audio)
if(MSVC)
    target_compile_options(mpo_audio_ppsspp_difftest PRIVATE /W0)
else()
    # PPSSPP's headers are included here too.
    target_compile_options(mpo_audio_ppsspp_difftest PRIVATE -w)
endif()
add_test(NAME mpo_audio_ppsspp_difftest COMMAND mpo_audio_ppsspp_difftest --seeds 300)
