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
# --- F07a: VFPU golden vectors and the CPU difftest harness -----------------
include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_cpu/ppsspp_vfpu_tables.cmake)
include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_cpu/ppsspp_cpu_ref.cmake)

add_library(mpo_difftest_lib STATIC
    difftest/case_gen.cpp
    difftest/difftest_core.cpp
    difftest/external_executor.cpp
    difftest/ref_interpreter.cpp
    golden/golden.cpp
    golden/vfpu_functions.cpp
)
target_include_directories(mpo_difftest_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpo_difftest_lib PUBLIC mpo_vfpu)
if(MSVC)
    # C4324: CpuState pads after the alignas(16) VFPU block on purpose. PUBLIC
    # because every target that includes difftest.hpp sees the same struct.
    target_compile_options(mpo_difftest_lib PUBLIC /wd4324)
else()
    target_compile_options(mpo_difftest_lib PRIVATE -ffp-contract=off)
endif()
mpo_target_warnings(mpo_difftest_lib)

# Golden check with mpo_vfpu alone: runs in CI without any PPSSPP code.
add_executable(mpo_vfpu_golden_test vfpu_golden_test.cpp)
target_link_libraries(mpo_vfpu_golden_test PRIVATE mpo_difftest_lib)
mpo_target_warnings(mpo_vfpu_golden_test)
add_test(NAME mpo_vfpu_golden_test
         COMMAND mpo_vfpu_golden_test --tables "${MPO_VFPU_TABLES_DIR}"
                 --golden ${CMAKE_CURRENT_SOURCE_DIR}/data/vfpu_golden.txt)
set_tests_properties(mpo_vfpu_golden_test PROPERTIES SKIP_RETURN_CODE 77)

if(MPO_BUILD_PPSSPP_REF)
    # PPSSPP's own interpreter, test-only. Upstream code: no project warnings,
    # no sanitizers (it relies on wrap-around and x86 conversions for some UB,
    # which -O0 keeps naive), and locals start at zero so the lanes it reads
    # uninitialised match mpo_vfpu.
    add_library(mpo_ppsspp_ref STATIC ${MPO_PPSSPP_CPU_SOURCES} ppsspp_ref/ppsspp_ref.cpp)
    target_include_directories(mpo_ppsspp_ref BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ppsspp_ref/shim)
    target_include_directories(mpo_ppsspp_ref SYSTEM PRIVATE ${MPO_PPSSPP_CPU_DIR})
    target_include_directories(mpo_ppsspp_ref PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mpo_ppsspp_ref PUBLIC mpo_difftest_lib)
    if(MSVC)
        message(FATAL_ERROR "mpo_ppsspp_ref needs GCC or Clang (-ftrivial-auto-var-init=zero)")
    else()
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-ftrivial-auto-var-init=zero MPO_HAVE_AUTO_VAR_INIT_ZERO)
        if(NOT MPO_HAVE_AUTO_VAR_INIT_ZERO)
            message(FATAL_ERROR "mpo_ppsspp_ref needs -ftrivial-auto-var-init=zero (GCC 12+, "
                                "Clang 16+); or configure with -DMPO_BUILD_PPSSPP_REF=OFF")
        endif()
        target_compile_options(mpo_ppsspp_ref PRIVATE -w -O0 -ffp-contract=off -fno-sanitize=all
                                                      -ftrivial-auto-var-init=zero)
    endif()

    find_package(Threads REQUIRED)
    add_executable(mpo_vfpu_golden_gen golden/golden_gen.cpp)
    target_link_libraries(mpo_vfpu_golden_gen PRIVATE mpo_ppsspp_ref Threads::Threads)
    mpo_target_warnings(mpo_vfpu_golden_gen)
    # The committed golden file must be exactly what PPSSPP produces today.
    add_test(NAME mpo_vfpu_golden_regen
             COMMAND mpo_vfpu_golden_gen --tables "${MPO_VFPU_TABLES_DIR}"
                     --check ${CMAKE_CURRENT_SOURCE_DIR}/data/vfpu_golden.txt)
endif()

add_executable(mpo_difftest difftest/difftest_main.cpp)
if(MPO_BUILD_PPSSPP_REF)
    target_link_libraries(mpo_difftest PRIVATE mpo_ppsspp_ref)
    target_compile_definitions(mpo_difftest PRIVATE MPO_HAVE_PPSSPP_REF=1)
else()
    target_link_libraries(mpo_difftest PRIVATE mpo_difftest_lib)
    target_compile_definitions(mpo_difftest PRIVATE MPO_HAVE_PPSSPP_REF=0)
endif()
mpo_target_warnings(mpo_difftest)

# The harness against a test executor (itself and a fault injector).
add_test(NAME mpo_difftest_selftest
         COMMAND mpo_difftest --selftest --class all --count 1500 --tables "${MPO_VFPU_TABLES_DIR}")
set_tests_properties(mpo_difftest_selftest PROPERTIES SKIP_RETURN_CODE 77)
if(MPO_BUILD_PPSSPP_REF)
    add_test(NAME mpo_difftest_ppsspp
             COMMAND mpo_difftest --b ppsspp --class all --count 20000
                     --tables "${MPO_VFPU_TABLES_DIR}")
    set_tests_properties(mpo_difftest_ppsspp PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Unicorn (GPL-2.0-only) only ever runs as a separate process from a venv
# outside the repo, e.g. -DMPO_UNICORN_PYTHON=$HOME/tools/venv-unicorn/bin/python.
set(MPO_UNICORN_PYTHON "" CACHE FILEPATH
    "Python with the unicorn module for the MIPS32 difftest (empty: the test is skipped)")
if(MPO_UNICORN_PYTHON)
    set(mpo_unicorn_command "\"${MPO_UNICORN_PYTHON}\" -I \"${CMAKE_CURRENT_SOURCE_DIR}/unicorn_exec.py\"")
else()
    set(mpo_unicorn_command "")
endif()
add_test(NAME mpo_difftest_unicorn
         COMMAND mpo_difftest --b unicorn --count 4000 --tables "${MPO_VFPU_TABLES_DIR}"
                 --unicorn-command "${mpo_unicorn_command}")
set_tests_properties(mpo_difftest_unicorn PROPERTIES SKIP_RETURN_CODE 77)

# mpo_apscan (F20a): virtual APs and the sceNetApctl scan model, no game data.
add_executable(mpo_apscan_test apscan_test.cpp)
target_link_libraries(mpo_apscan_test PRIVATE mpo_apscan)
mpo_target_warnings(mpo_apscan_test)
add_test(NAME mpo_apscan_test COMMAND mpo_apscan_test)

# --- F10a: window, presentation and input -----------------------------------
# mpo_frontend without SDL; color.hpp from mpo_ge is the reference converter.
add_executable(mpo_frontend_test frontend_test.cpp)
target_link_libraries(mpo_frontend_test PRIVATE mpo_frontend mpo_ge)
mpo_target_warnings(mpo_frontend_test)
add_test(NAME mpo_frontend_test COMMAND mpo_frontend_test)
if(TARGET mpo_frontend_sdl)
    # Headless: SDL's dummy video driver and software renderer, a virtual
    # gamepad and synthetic events. No display, no real device.
    add_executable(mpo_frontend_sdl_test frontend_sdl_test.cpp)
    target_link_libraries(mpo_frontend_sdl_test PRIVATE mpo_frontend_sdl)
    mpo_target_warnings(mpo_frontend_sdl_test)
    add_test(NAME mpo_frontend_sdl_test
             COMMAND mpo_frontend_sdl_test ${CMAKE_CURRENT_SOURCE_DIR}/../config/input.toml)
    # The manual tool end to end (four test cards, scripted input), headless.
    add_test(NAME mpo_frontend_demo_smoke
             COMMAND mpo_frontend_demo --hidden --auto --seconds-per-view 0.3 --synthetic-input
                     --renderer software
                     --trace ${CMAKE_CURRENT_BINARY_DIR}/frontend_demo_trace.txt)
    set_tests_properties(mpo_frontend_demo_smoke PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
