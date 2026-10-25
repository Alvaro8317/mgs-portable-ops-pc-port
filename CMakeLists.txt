# SPDX-License-Identifier: GPL-3.0-or-later
#
# Root build: PSPRecomp framework (framework/psprecomp, MIT) plus the MPO
# profile (profiles/mpo). The framework must keep building without the game.

cmake_minimum_required(VERSION 3.22)
project(MpoPcPort VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# PSPRecomp only looks for profiles inside its own tree, so the root owns the
# selection: the framework is always configured framework-only and our profile
# is added from profiles/<name> here.
set(PSPRECOMP_PROFILE "mpo" CACHE STRING
    "Profile to build from profiles/<name>; empty builds only the framework")
option(MPO_USE_CCACHE "Use ccache as the compiler launcher when available" ON)
option(MPO_WARNINGS_AS_ERRORS "Treat warnings in project (non-framework) code as errors" OFF)
set(MPO_SANITIZERS "" CACHE STRING
    "Sanitizers for the whole build, e.g. \"address;undefined\" (GCC/Clang only)")

if(MPO_USE_CCACHE)
    find_program(MPO_CCACHE_PROGRAM ccache)
    if(MPO_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MPO_CCACHE_PROGRAM}")
    endif()
endif()

if(MPO_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "MPO_SANITIZERS is only supported with GCC and Clang")
    endif()
    list(JOIN MPO_SANITIZERS "," mpo_sanitizer_list)
    # Applied before add_subdirectory() so the framework is instrumented too.
    add_compile_options(-fsanitize=${mpo_sanitizer_list} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${mpo_sanitizer_list})
endif()

# Warning flags for project targets. The framework keeps its own flags.
function(mpo_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(MPO_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(MPO_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

enable_testing()

set(mpo_selected_profile "${PSPRECOMP_PROFILE}")
set(PSPRECOMP_PROFILE "")
add_subdirectory(framework/psprecomp)
set(PSPRECOMP_PROFILE "${mpo_selected_profile}")

# Consumers see the framework headers as system headers, so warnings that live
# upstream do not break MPO_WARNINGS_AS_ERRORS in project code. The framework
# itself still compiles with its own warning flags.
get_target_property(mpo_framework_includes psprecomp_core INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET psprecomp_core APPEND PROPERTY
             INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${mpo_framework_includes})

find_package(Python3 3.10 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME spdx_check
             COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_spdx.py
                     --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME spdx_check_unittest
             COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_spdx_test.py)
endif()

if(NOT PSPRECOMP_PROFILE STREQUAL "")
    set(mpo_profile_dir "${CMAKE_CURRENT_SOURCE_DIR}/profiles/${PSPRECOMP_PROFILE}")
    if(NOT EXISTS "${mpo_profile_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Unknown profile: ${PSPRECOMP_PROFILE} (no ${mpo_profile_dir})")
    endif()
    add_subdirectory("${mpo_profile_dir}" "profiles/${PSPRECOMP_PROFILE}")
endif()
