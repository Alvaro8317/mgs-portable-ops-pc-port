# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_texreplace (F24c): texture replacement packs (textures.ini + PNG, in
# PPSSPP's format) and the texture dump, as the GE's TextureProvider. The key
# hashes with xxHash (third_party/xxhash, BSD-2-Clause); PNGs go through
# SDL3's own loader and writer when SDL3 has video (without it, the packs
# cannot be used and say so). F26a: the PNGs load on worker threads
# (decode_pool), with their mip levels.

enable_language(C)
add_library(mpo_xxhash STATIC ${PROJECT_SOURCE_DIR}/third_party/xxhash/xxhash.c)
target_include_directories(mpo_xxhash SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/xxhash)

find_package(Threads REQUIRED)
add_library(mpo_texreplace STATIC
    dump_index.cpp
    decode_pool.cpp
    png_io.cpp
    texture_key.cpp
    texture_replacer.cpp
    textures_ini.cpp
)
target_include_directories(mpo_texreplace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_texreplace PUBLIC mpo_ge Threads::Threads PRIVATE mpo_xxhash)
if(TARGET mpo::sdl3 AND NOT MPO_SDL3_AUDIO_ONLY)
    target_link_libraries(mpo_texreplace PRIVATE mpo::sdl3)
    target_compile_definitions(mpo_texreplace PRIVATE MPO_TEXREPLACE_SDL=1)
else()
    target_compile_definitions(mpo_texreplace PRIVATE MPO_TEXREPLACE_SDL=0)
endif()
mpo_target_warnings(mpo_texreplace)
