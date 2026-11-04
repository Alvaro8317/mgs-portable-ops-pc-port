# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_prompticons (F25a): the game's button icons follow the last device. A
# TextureProvider in front of the player's pack (mpo_texreplace, F24c) that
# draws the port's own icons (icon_art.cpp) over the textures listed in
# config/prompt_icons.toml, which is built into the library (only keys and
# numbers; no game art, CLAUDE.md §1). No SDL: tested without a window.

set(mpo_prompt_icons_toml ${CMAKE_CURRENT_SOURCE_DIR}/../../config/prompt_icons.toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${mpo_prompt_icons_toml})
file(READ ${mpo_prompt_icons_toml} MPO_PROMPT_ICONS_TOML)
string(FIND "${MPO_PROMPT_ICONS_TOML}" ")mpo_toml" mpo_prompt_icons_delimiter)
if(NOT mpo_prompt_icons_delimiter EQUAL -1)
    message(FATAL_ERROR "prompt_icons.toml must not contain )mpo_toml")
endif()
configure_file(builtin_inventory.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/builtin_inventory.cpp @ONLY)

add_library(mpo_prompticons STATIC
    icon_art.cpp
    icon_inventory.cpp
    prompt_icons.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/builtin_inventory.cpp
)
target_include_directories(mpo_prompticons PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mpo_prompticons SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include)
target_compile_definitions(mpo_prompticons PRIVATE TOML_EXCEPTIONS=0)
target_link_libraries(mpo_prompticons PUBLIC mpo_texreplace mpo_frontend mpo_ge)
mpo_target_warnings(mpo_prompticons)
