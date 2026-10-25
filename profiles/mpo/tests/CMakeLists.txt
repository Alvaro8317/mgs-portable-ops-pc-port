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

add_executable(mpo_crypto_test crypto_test.cpp)
target_link_libraries(mpo_crypto_test PRIVATE mpo_host)
mpo_target_warnings(mpo_crypto_test)
add_test(NAME mpo_crypto_test COMMAND mpo_crypto_test)

# Python tools in profiles/mpo/tools: tests with synthetic data only.
if(Python3_FOUND)
    foreach(mpo_tool_test IN ITEMS tools_test zstd_lite_test ge_usage_test)
        add_test(NAME mpo_${mpo_tool_test}
                 COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/${mpo_tool_test}.py)
    endforeach()
    foreach(mpo_tool_test IN ITEMS psp_prx_test zar_test rlc_test)
        add_test(NAME mpo_${mpo_tool_test}
                 COMMAND Python3::Interpreter -I
                         ${CMAKE_CURRENT_SOURCE_DIR}/../tools/${mpo_tool_test}.py)
    endforeach()
endif()
