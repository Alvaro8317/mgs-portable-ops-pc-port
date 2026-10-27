# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_hle (F08b): mpo_kernel on the PSPRecomp CPU (one dispatch loop per PSP
# thread fiber), the HLE import table (histogram, trace, visible failure of
# unimplemented NIDs), the kernel NIDs and the module loader; F09 adds IO on
# mpo_io, the game clock and RTC, the vblank (sceDisplay) and interrupts.
# Like mpo_recomp it only sees the framework headers: whatever links it also
# links psprecomp_core (an object library) once.

add_library(mpo_hle STATIC
    cpu_bridge.cpp
    game_clock.cpp
    hle_display.cpp
    hle_io.cpp
    hle_kernel.cpp
    hle_system.cpp
    hle_table.cpp
    hle_time.cpp
    modules.cpp
    runtime_memory.cpp
    system.cpp
)
target_include_directories(mpo_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(mpo_hle SYSTEM PUBLIC
    $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(mpo_hle PUBLIC mpo_kernel mpo_io)
if(MSVC)
    # C4324: CpuBridge keeps PSPRecomp's alignas(16) AllegrexContext per thread.
    target_compile_options(mpo_hle PUBLIC /wd4324)
endif()
mpo_target_warnings(mpo_hle)

# The resident Konami modules, recompiled from the user's files like mgp_main
# (generate.py --module sound2|kjfs|zlibdec writes <dir>/<module>/generated).
set(MPO_GENERATED_MODULES_DIR "" CACHE PATH
    "generate.py output root with <module>/generated for sound2, kjfs and zlibdec; empty: not built")

# mpo_boot: boots mgp_main on the kernel and traces it (needs the corpus).
if(TARGET mpo_generated_mgp_main)
    include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_cpu/ppsspp_vfpu_tables.cmake)
    add_executable(mpo_boot boot_main.cpp)
    target_link_libraries(mpo_boot PRIVATE psprecomp_core mpo_hle mpo_recomp mpo_generated_mgp_main)
    target_compile_definitions(mpo_boot PRIVATE MPO_DEFAULT_VFPU_TABLES="${MPO_VFPU_TABLES_DIR}")
    mpo_target_warnings(mpo_boot)
    foreach(mpo_module IN ITEMS sound2 kjfs zlibdec)
        set(mpo_module_dir "${MPO_GENERATED_MODULES_DIR}/${mpo_module}/generated")
        if(NOT MPO_GENERATED_MODULES_DIR OR NOT EXISTS "${mpo_module_dir}/${mpo_module}_registry.cpp")
            continue()
        endif()
        file(GLOB mpo_module_sources CONFIGURE_DEPENDS "${mpo_module_dir}/*.cpp")
        message(STATUS "${mpo_module} corpus: ${mpo_module_dir}")
        add_library(mpo_generated_${mpo_module} OBJECT ${mpo_module_sources})
        target_include_directories(mpo_generated_${mpo_module} PRIVATE "${mpo_module_dir}")
        target_include_directories(mpo_generated_${mpo_module} SYSTEM PRIVATE
            $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
        # Same build rules as the mgp_main corpus (recomp/CMakeLists.txt).
        set_target_properties(mpo_generated_${mpo_module} PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
        if(MSVC)
            target_compile_options(mpo_generated_${mpo_module} PRIVATE /W0 /bigobj)
        else()
            target_compile_options(mpo_generated_${mpo_module} PRIVATE -w -ffp-contract=off
                                   -fno-sanitize=all -O${PSPRECOMP_GENERATED_OPT_LEVEL})
        endif()
        string(TOUPPER "${mpo_module}" mpo_module_upper)
        target_link_libraries(mpo_boot PRIVATE mpo_generated_${mpo_module})
        target_compile_definitions(mpo_boot PRIVATE MPO_CORPUS_${mpo_module_upper}=1)
    endforeach()
endif()
