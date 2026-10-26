# SPDX-License-Identifier: GPL-3.0-or-later
#
# MPO profile. Everything that depends on a game address, hash or layout lives
# under this directory (CLAUDE.md §1.3). Generated code (generated/) is built
# from the user's own executable and is never versioned.

add_subdirectory(host)
add_subdirectory(tools)

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    add_subdirectory(tests)
endif()
