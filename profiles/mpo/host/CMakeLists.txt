# SPDX-License-Identifier: GPL-3.0-or-later

include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_crypto/ppsspp_crypto.cmake)

add_subdirectory(kernel)
add_subdirectory(vfpu)
add_subdirectory(recomp)

# The disc readers on their own, so the runtime (mpo_io) can read the ISO
# without linking the installer and its PRX decrypter.
add_library(mpo_disc STATIC
    disc/iso9660.cpp
    disc/param_sfo.cpp
)
target_include_directories(mpo_disc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
mpo_target_warnings(mpo_disc)

add_library(mpo_host STATIC
    crypto/psp_module.cpp
    install/installer.cpp
    install/save_profile.cpp
    install/supported_versions.cpp
    util/sha256.cpp
)
target_include_directories(mpo_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_host PUBLIC mpo_disc)
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
    MPO_DEFAULT_SUPPORTED_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/supported.toml"
    MPO_DEFAULT_SAVEDATA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../config/savedata.toml")
mpo_target_warnings(mpo)

add_subdirectory(ge)

# F24c: texture replacement packs and the texture dump (mpo_texreplace).
add_subdirectory(texreplace)

# F20a: AP Scan model (sceNetApctl) and virtual access points.
add_subdirectory(apscan)

add_subdirectory(frontend)

# F25a: the game's button icons by device (needs texreplace and frontend).
add_subdirectory(prompticons)

# F17a: PMF videos and PGF fonts (they use mpo_audio and mpo_ge_replay).
add_subdirectory(video)
add_subdirectory(font)

# F14a: Vulkan GE renderer (mpo_vulkan) and --renderer=vulkan in mpo_ppdmp_replay.
add_subdirectory(vulkan)

# F09: the PSP file systems (ISO, memory stick, devices) for sceIo*.
add_subdirectory(io)

# F08b: the kernel HLE on the recompiled CPU, the module loader and mpo_boot.
add_subdirectory(hle)

# F18a: sceUtility savedata, message and OSK dialogs (mpo_savedata).
add_subdirectory(savedata)

# F18c: the savedata cipher on sceChnnlsv and libkirk, for the save converter
# of the installer (mpo --import-save/--export-save). It holds keys, so only
# `mpo` and its test link it; the runtime saves plain.
target_link_libraries(mpo PRIVATE mpo_savedata)
if(MPO_ENABLE_PRX_DECRYPT)
    add_library(mpo_savedata_cipher STATIC crypto/savedata_cipher_kirk.cpp)
    target_include_directories(mpo_savedata_cipher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mpo_savedata_cipher PUBLIC mpo_savedata PRIVATE mpo_chnnlsv)
    mpo_target_warnings(mpo_savedata_cipher)
    target_link_libraries(mpo PRIVATE mpo_savedata_cipher)
    target_compile_definitions(mpo PRIVATE MPO_HAVE_SAVEDATA_CIPHER=1)
else()
    target_compile_definitions(mpo PRIVATE MPO_HAVE_SAVEDATA_CIPHER=0)
endif()
