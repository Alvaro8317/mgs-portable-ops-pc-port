# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_widescreen (F25b): --aspect, the x scale of the camera's projection in
# the module (from config/widescreen.toml, embedded here as a string so the
# binary needs no file at run time) and which flips are video frames.

set(mpo_widescreen_config "${CMAKE_CURRENT_SOURCE_DIR}/../../config/widescreen.toml")
file(READ "${mpo_widescreen_config}" MPO_WIDESCREEN_TOML)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mpo_widescreen_config}")
configure_file(widescreen_profile.inc.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/widescreen_profile.inc" @ONLY)

add_library(mpo_widescreen STATIC widescreen.cpp)
target_include_directories(mpo_widescreen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(mpo_widescreen SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include)
target_compile_definitions(mpo_widescreen PRIVATE TOML_EXCEPTIONS=0)
target_link_libraries(mpo_widescreen PRIVATE mpo_sha256)
mpo_target_warnings(mpo_widescreen)
