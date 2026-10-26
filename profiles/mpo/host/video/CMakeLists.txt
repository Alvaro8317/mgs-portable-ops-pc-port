# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_video (F17a): PSMF header, MPEG-PS demuxer and the media engine (H.264
# through FFmpeg, ATRAC3plus through mpo_audio), ported from PPSSPP v1.20.4.
# No HLE registration here (that is F17b). Without FFmpeg (MPO_FFMPEG=off or
# not found) the library still builds: header, demuxer and audio work and
# stepVideo() only advances the timestamp, like PPSSPP without USE_FFMPEG.

include(${PROJECT_SOURCE_DIR}/third_party/ffmpeg/ffmpeg.cmake)

add_library(mpo_video STATIC
    media_engine.cpp
    mpeg_demux.cpp
    psmf.cpp
)
target_include_directories(mpo_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_video PUBLIC mpo_audio)
if(TARGET mpo::ffmpeg)
    target_link_libraries(mpo_video PRIVATE mpo::ffmpeg)
    target_compile_definitions(mpo_video PRIVATE MPO_HAVE_FFMPEG=1)
else()
    target_compile_definitions(mpo_video PRIVATE MPO_HAVE_FFMPEG=0)
endif()
mpo_target_warnings(mpo_video)

# Local analysis tool: decodes .pmf files and dumps frames (PNG) and audio (WAV).
add_executable(mpo_pmf_decode pmf_decode.cpp)
target_link_libraries(mpo_pmf_decode PRIVATE mpo_video mpo_ge_replay)
mpo_target_warnings(mpo_pmf_decode)
