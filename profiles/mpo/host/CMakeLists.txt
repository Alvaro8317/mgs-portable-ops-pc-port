# SPDX-License-Identifier: GPL-3.0-or-later

include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_crypto/ppsspp_crypto.cmake)

add_library(mpo_host STATIC
    crypto/psp_module.cpp
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
if(MPO_ENABLE_PRX_DECRYPT)
    # PRIVATE: the decrypter (and the keys inside it) reaches only the targets
    # that link mpo_host, i.e. the installer and its tests.
    target_link_libraries(mpo_host PRIVATE mpo_prx_decrypter)
    target_compile_definitions(mpo_host PRIVATE MPO_HAVE_PRX_DECRYPT=1)
else()
    target_compile_definitions(mpo_host PRIVATE MPO_HAVE_PRX_DECRYPT=0)
endif()
mpo_target_warnings(mpo_host)

add_subdirectory(audio)

add_executable(mpo main.cpp)
target_link_libraries(mpo PRIVATE mpo_host)
target_compile_definitions(mpo PRIVATE
    MPO_DEFAULT_SUPPORTED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/supported.toml")
mpo_target_warnings(mpo)
