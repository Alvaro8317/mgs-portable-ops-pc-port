# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_io (F09): the PSP file systems (ISO, memory stick directory, device
# table) behind IoFileMgrForUser. No HLE and no game data: the sceIo* NIDs
# that use it are in hle/hle_io.cpp.

add_library(mpo_io STATIC
    directory_file_system.cpp
    file_system.cpp
    iso_file_system.cpp
    meta_file_system.cpp
)
# Headers are included as "io/<name>.hpp"; the ISO reader from mpo_disc.
target_include_directories(mpo_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mpo_io PUBLIC mpo_disc)
mpo_target_warnings(mpo_io)
