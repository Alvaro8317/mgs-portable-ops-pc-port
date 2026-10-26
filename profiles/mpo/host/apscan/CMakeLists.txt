# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_apscan (F20a): sceNetApctl scan model and seeded virtual access points
# for MPO's AP Scan. No game data, no NID registration (that is F20b); see
# apctl_model.hpp and docs/analysis/apscan.md.

add_library(mpo_apscan STATIC
    apctl_model.cpp
    scan_source.cpp
    virtual_ap.cpp
)
# Headers are included as "apscan/<name>.hpp".
target_include_directories(mpo_apscan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Only for kernel/clock.hpp (the injectable PSP clock).
target_link_libraries(mpo_apscan PUBLIC mpo_kernel)
mpo_target_warnings(mpo_apscan)
