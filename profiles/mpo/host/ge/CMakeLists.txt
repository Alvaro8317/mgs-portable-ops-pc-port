# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_ge: the GE (registers, display list interpreter, software renderer
# ported from PPSSPP's softgpu). Depends on nothing but the C++ library, so
# F11b can link it into the runtime and drive it from sceGe*.
#
# mpo_ge_replay + mpo_ppdmp_replay: frame dump (.ppdmp) reader and replayer,
# for development only (they pull in the vendored zstd decompressor).

add_library(mpo_ge STATIC
    ge.cpp
    ge_state.cpp
    memory_view.cpp
    pixel.cpp
    primitive_assembler.cpp
    rasterizer.cpp
    sampler.cpp
    vertex_decoder.cpp
)
target_include_directories(mpo_ge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mpo_ge PUBLIC cxx_std_20)
# Pixel-exact float maths: never fuse a*b+c, as PPSSPP's x86-64 build does
# not (MSVC's default /fp:precise does not contract either).
if(NOT MSVC)
    target_compile_options(mpo_ge PRIVATE -ffp-contract=off)
endif()
mpo_target_warnings(mpo_ge)

# Zstandard decompressor (third_party/zstd, BSD-3-Clause), upstream code with
# its own warnings.
enable_language(C)
add_library(mpo_zstd_decompress STATIC ${PROJECT_SOURCE_DIR}/third_party/zstd/zstddeclib.c)
target_include_directories(mpo_zstd_decompress SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/zstd)

add_library(mpo_ge_replay STATIC
    replay/dump_replayer.cpp
    replay/image.cpp
    replay/ppdmp.cpp
)
target_include_directories(mpo_ge_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/replay)
target_link_libraries(mpo_ge_replay PUBLIC mpo_ge PRIVATE mpo_zstd_decompress)
mpo_target_warnings(mpo_ge_replay)

add_executable(mpo_ppdmp_replay tools/ppdmp_replay.cpp)
target_link_libraries(mpo_ppdmp_replay PRIVATE mpo_ge_replay)
mpo_target_warnings(mpo_ppdmp_replay)
