# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(mpo_profile_smoke_test smoke_test.cpp)
target_link_libraries(mpo_profile_smoke_test PRIVATE psprecomp_core)
mpo_target_warnings(mpo_profile_smoke_test)
add_test(NAME mpo_profile_smoke_test COMMAND mpo_profile_smoke_test)

add_executable(mpo_disc_test disc_test.cpp synthetic_disc.cpp)
target_link_libraries(mpo_disc_test PRIVATE mpo_host)
target_compile_definitions(mpo_disc_test PRIVATE
    MPO_SUPPORTED_TOML="${CMAKE_CURRENT_SOURCE_DIR}/../config/supported.toml")
mpo_target_warnings(mpo_disc_test)
add_test(NAME mpo_disc_test COMMAND mpo_disc_test)
