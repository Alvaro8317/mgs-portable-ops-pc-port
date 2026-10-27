# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_savedata (F18a): sceUtilitySavedata, sceUtilityMsgDialog and
# sceUtilityOsk without the HLE: dialog state machines ported from PPSSPP,
# the savedata files on the host and the default ms0:/ folder. No game data,
# no NID registration (that is F18b), no libkirk (see savedata_cipher.hpp).

add_library(mpo_savedata STATIC
    dialog.cpp
    msg_dialog.cpp
    osk_dialog.cpp
    save_dialog.cpp
    save_paths.cpp
    savedata_store.cpp
    sfo_file.cpp
    utility_dialogs.cpp
)
# Headers are included as "savedata/<name>.hpp".
target_include_directories(mpo_savedata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# kernel/clock.hpp and kernel/guest_memory.hpp (FlatGuestMemory in tests).
target_link_libraries(mpo_savedata PUBLIC mpo_kernel)
mpo_target_warnings(mpo_savedata)
