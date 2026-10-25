# SPDX-License-Identifier: GPL-3.0-or-later

add_library(mpo_host STATIC
    disc/iso9660.cpp
    disc/param_sfo.cpp
    install/installer.cpp
    install/supported_versions.cpp
    util/sha256.cpp
)
target_include_directories(mpo_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mpo_host SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/tomlplusplus/include)
# Errors come back as toml::parse_result instead of exceptions.
target_compile_definitions(mpo_host PRIVATE TOML_EXCEPTIONS=0)
mpo_target_warnings(mpo_host)

add_executable(mpo main.cpp)
target_link_libraries(mpo PRIVATE mpo_host)
target_compile_definitions(mpo PRIVATE
    MPO_DEFAULT_SUPPORTED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/supported.toml")
mpo_target_warnings(mpo)
