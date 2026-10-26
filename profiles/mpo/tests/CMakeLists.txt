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

# mpo_audio (F16a): synthetic data and golden vectors only.
foreach(mpo_audio_test IN ITEMS audio_sas_test audio_atrac_test audio_output_test)
    add_executable(mpo_${mpo_audio_test} ${mpo_audio_test}.cpp)
    target_link_libraries(mpo_${mpo_audio_test} PRIVATE mpo_audio)
    mpo_target_warnings(mpo_${mpo_audio_test})
    add_test(NAME mpo_${mpo_audio_test} COMMAND mpo_${mpo_audio_test})
endforeach()
if(TARGET mpo_audio_sdl)
    target_link_libraries(mpo_audio_output_test PRIVATE mpo_audio_sdl)
    target_compile_definitions(mpo_audio_output_test PRIVATE MPO_HAVE_SDL_AUDIO=1)
endif()
option(MPO_AUDIO_PPSSPP_DIFFTEST
       "Build mpo_audio_ppsspp_difftest from the PPSSPP sources in MPO_PPSSPP_SOURCE_DIR" OFF)
if(MPO_AUDIO_PPSSPP_DIFFTEST)
    add_subdirectory(audio_ppsspp_ref)
endif()

# Python tools in profiles/mpo/tools: tests with synthetic data only.
if(Python3_FOUND)
    foreach(mpo_tool_test IN ITEMS tools_test zstd_lite_test ge_usage_test ge_frame_diff_test)
        add_test(NAME mpo_${mpo_tool_test}
                 COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/${mpo_tool_test}.py)
    endforeach()
    # F06: Ghidra/PSPRecomp comparison and hints, end to end through the
    # C++ exporter on a synthetic PRX.
    add_test(NAME mpo_recomp_tools_test
             COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/recomp_tools_test.py)
    set_tests_properties(mpo_recomp_tools_test PROPERTIES
        ENVIRONMENT "MPO_ANALYSIS_EXPORT=$<TARGET_FILE:mpo_analysis_export>")
    foreach(mpo_tool_test IN ITEMS psp_prx_test zar_test rlc_test)
        add_test(NAME mpo_${mpo_tool_test}
                 COMMAND Python3::Interpreter -I
                         ${CMAKE_CURRENT_SOURCE_DIR}/../tools/${mpo_tool_test}.py)
    endforeach()
endif()

add_executable(mpo_kernel_test kernel_test.cpp)
target_link_libraries(mpo_kernel_test PRIVATE mpo_kernel)
mpo_target_warnings(mpo_kernel_test)
add_test(NAME mpo_kernel_test COMMAND mpo_kernel_test)
# GE: list interpreter, software renderer and .ppdmp replay on synthetic data.
add_executable(mpo_ge_test ge_test.cpp)
target_link_libraries(mpo_ge_test PRIVATE mpo_ge_replay)
mpo_target_warnings(mpo_ge_test)
add_test(NAME mpo_ge_test COMMAND mpo_ge_test)
add_test(NAME mpo_ppdmp_replay_usage COMMAND mpo_ppdmp_replay)
set_tests_properties(mpo_ppdmp_replay_usage PROPERTIES WILL_FAIL TRUE)
