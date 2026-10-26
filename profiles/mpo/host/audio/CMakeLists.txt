# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_audio: SAS mixer, sceAtrac contexts (on at3_standalone) and the sceAudio
# channels, ported from PPSSPP v1.20.4 and kept free of HLE registration
# (that is F16b). mpo_audio_sdl adds the SDL3 device output.

include(${PROJECT_SOURCE_DIR}/third_party/at3_standalone/at3_standalone.cmake)
include(${PROJECT_SOURCE_DIR}/third_party/sdl3/sdl3.cmake)

add_library(mpo_audio STATIC
    atrac_context.cpp
    atrac_decoder.cpp
    atrac_manager.cpp
    atrac_track.cpp
    audio_channels.cpp
    sas_audio.cpp
    sas_core.cpp
    sas_reverb.cpp
    stereo_resampler.cpp
)
target_include_directories(mpo_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_audio PRIVATE mpo_at3_standalone)
mpo_target_warnings(mpo_audio)
# The ported code keeps PPSSPP's integer types (u8/s16/int mixes) so it stays
# easy to diff against upstream; the narrowing it does is intended.
if(MSVC)
    target_compile_options(mpo_audio PRIVATE /wd4018 /wd4146 /wd4244 /wd4245 /wd4267 /wd4310 /wd4389)
else()
    target_compile_options(mpo_audio PRIVATE -Wno-conversion -Wno-sign-conversion)
endif()

add_executable(mpo_at3_to_wav at3_to_wav.cpp)
target_link_libraries(mpo_at3_to_wav PRIVATE mpo_audio)
mpo_target_warnings(mpo_at3_to_wav)

if(TARGET mpo::sdl3)
    add_library(mpo_audio_sdl STATIC sdl_audio_output.cpp)
    target_include_directories(mpo_audio_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mpo_audio_sdl PUBLIC mpo_audio PRIVATE mpo::sdl3)
    mpo_target_warnings(mpo_audio_sdl)
endif()
