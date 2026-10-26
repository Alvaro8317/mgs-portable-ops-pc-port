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

# --- F07b: the PSPRecomp emitter and interpreter under the difftest ---------
# mpo_aot_casegen lays the cases out as guest code (difftest/aot_layout.hpp),
# psp_recomp recompiles them at build time exactly like the game
# (--auto --hints --vfpu-executor), and mpo_difftest_aot runs the generated
# code, and the framework's interpreter fallback, against the references.
set(MPO_AOT_DIFFTEST_COUNT "1000" CACHE STRING
    "Cases per instruction class compiled into mpo_difftest_aot")
set(MPO_AOT_DIFFTEST_SEED "5066831" CACHE STRING
    "Seed of the cases compiled into mpo_difftest_aot (0x4D504F, mpo_difftest's default)")
if(NOT MPO_AOT_DIFFTEST_COUNT MATCHES "^[1-9][0-9]*$" OR NOT MPO_AOT_DIFFTEST_SEED MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MPO_AOT_DIFFTEST_COUNT and MPO_AOT_DIFFTEST_SEED must be decimal integers")
endif()

add_executable(mpo_aot_casegen difftest/aot_casegen.cpp difftest/aot_layout.cpp)
target_link_libraries(mpo_aot_casegen PRIVATE mpo_difftest_lib)
mpo_target_warnings(mpo_aot_casegen)

# 8 classes (all_classes()), one 32-byte slot per case, 512 slots per 16 KiB
# unit: the file names are known before psp_recomp runs (mpo_aot_casegen
# --expect-units fails the build if that ever stops being true).
math(EXPR mpo_aot_units "(8 * ${MPO_AOT_DIFFTEST_COUNT} + 511) / 512")
set(mpo_aot_dir ${CMAKE_CURRENT_BINARY_DIR}/aot_cases)
set(mpo_aot_sources ${mpo_aot_dir}/generated/generated_registry.cpp)
math(EXPR mpo_aot_last_unit "${mpo_aot_units} - 1")
foreach(mpo_aot_unit RANGE ${mpo_aot_last_unit})
    string(LENGTH "${mpo_aot_unit}" mpo_aot_digits)
    math(EXPR mpo_aot_pad "4 - ${mpo_aot_digits}")
    string(REPEAT "0" ${mpo_aot_pad} mpo_aot_zeros)
    list(APPEND mpo_aot_sources ${mpo_aot_dir}/generated/generated_unit_${mpo_aot_zeros}${mpo_aot_unit}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${mpo_aot_dir}/generated.stamp
    BYPRODUCTS ${mpo_aot_sources} ${mpo_aot_dir}/cases.elf ${mpo_aot_dir}/cases.hints
    COMMAND ${CMAKE_COMMAND} -E make_directory ${mpo_aot_dir}/generated
    COMMAND mpo_aot_casegen --seed ${MPO_AOT_DIFFTEST_SEED} --count ${MPO_AOT_DIFFTEST_COUNT}
            --elf ${mpo_aot_dir}/cases.elf --hints ${mpo_aot_dir}/cases.hints
            --expect-units ${mpo_aot_units}
    COMMAND psp_recomp ${mpo_aot_dir}/cases.elf --auto ${mpo_aot_dir}/generated 0x08A00000 0x4000
            --hints ${mpo_aot_dir}/cases.hints --vfpu-executor
    COMMAND ${CMAKE_COMMAND} -E touch ${mpo_aot_dir}/generated.stamp
    DEPENDS mpo_aot_casegen psp_recomp
    COMMENT "Recompiling the difftest cases with psp_recomp"
    VERBATIM)
add_custom_target(mpo_aot_cases DEPENDS ${mpo_aot_dir}/generated.stamp)

# The generated cases: no project warnings (so no /W4 vs /W0 clash on MSVC),
# no float contraction (bit-exactness).
add_library(mpo_aot_cases_obj OBJECT ${mpo_aot_sources})
add_dependencies(mpo_aot_cases_obj mpo_aot_cases)
target_include_directories(mpo_aot_cases_obj SYSTEM PRIVATE
    $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
set_source_files_properties(${mpo_aot_sources} PROPERTIES GENERATED TRUE)
if(MSVC)
    target_compile_options(mpo_aot_cases_obj PRIVATE /W0 /bigobj)
else()
    target_compile_options(mpo_aot_cases_obj PRIVATE -w -ffp-contract=off)
endif()

add_executable(mpo_difftest_aot difftest/difftest_main.cpp difftest/aot_executors.cpp
               difftest/aot_layout.cpp)
target_link_libraries(mpo_difftest_aot PRIVATE psprecomp_core mpo_recomp mpo_aot_cases_obj)
if(MPO_BUILD_PPSSPP_REF)
    target_link_libraries(mpo_difftest_aot PRIVATE mpo_ppsspp_ref)
    target_compile_definitions(mpo_difftest_aot PRIVATE MPO_HAVE_PPSSPP_REF=1)
else()
    target_link_libraries(mpo_difftest_aot PRIVATE mpo_difftest_lib)
    target_compile_definitions(mpo_difftest_aot PRIVATE MPO_HAVE_PPSSPP_REF=0)
endif()
target_compile_definitions(mpo_difftest_aot PRIVATE MPO_HAVE_AOT=1
    MPO_AOT_CASE_COUNT=${MPO_AOT_DIFFTEST_COUNT} MPO_AOT_CASE_SEED=${MPO_AOT_DIFFTEST_SEED}u)
mpo_target_warnings(mpo_difftest_aot)

set(mpo_aot_args --count ${MPO_AOT_DIFFTEST_COUNT} --seed ${MPO_AOT_DIFFTEST_SEED}
    --tables "${MPO_VFPU_TABLES_DIR}")
add_test(NAME mpo_difftest_emitter COMMAND mpo_difftest_aot --a emitter --b mpo --class all ${mpo_aot_args})
add_test(NAME mpo_difftest_interpreter COMMAND mpo_difftest_aot --a interp --b mpo --class all ${mpo_aot_args})
add_test(NAME mpo_difftest_emitter_unicorn
         COMMAND mpo_difftest_aot --a emitter --b unicorn ${mpo_aot_args}
                 --unicorn-command "${mpo_unicorn_command}")
set_tests_properties(mpo_difftest_emitter mpo_difftest_interpreter mpo_difftest_emitter_unicorn
                     PROPERTIES SKIP_RETURN_CODE 77)
if(MPO_BUILD_PPSSPP_REF)
    add_test(NAME mpo_difftest_emitter_ppsspp
             COMMAND mpo_difftest_aot --a emitter --b ppsspp --class all ${mpo_aot_args})
    set_tests_properties(mpo_difftest_emitter_ppsspp PROPERTIES SKIP_RETURN_CODE 77)
endif()
