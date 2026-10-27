# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_hle (F08b): mpo_kernel on the PSPRecomp CPU (one dispatch loop per PSP
# thread fiber), the HLE import table (histogram, trace, visible failure of
# unimplemented NIDs), the kernel NIDs and the module loader; F09 adds IO on
# mpo_io, the game clock and RTC, the vblank (sceDisplay) and interrupts;
# F10b, sceCtrl; F11b, sceGe on mpo_ge and sceMpeg on mpo_video; F13, the
# stage overlays (OverlayManager and their libraries).
# Like mpo_recomp it only sees the framework headers: whatever links it also
# links psprecomp_core (an object library) once.

add_library(mpo_hle STATIC
    cpu_bridge.cpp
    game_clock.cpp
    hle_ctrl.cpp
    hle_display.cpp
    hle_ge.cpp
    hle_io.cpp
    hle_kernel.cpp
    hle_mpeg.cpp
    hle_system.cpp
    hle_table.cpp
    hle_time.cpp
    modules.cpp
    overlays.cpp
    runtime_memory.cpp
    system.cpp
)
target_include_directories(mpo_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(mpo_hle SYSTEM PUBLIC
    $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
# mpo_frontend (F10a, no SDL): SceCtrlData and the button masks for sceCtrl.
target_link_libraries(mpo_hle PUBLIC mpo_kernel mpo_io mpo_frontend mpo_ge mpo_video)
# dlopen for the overlay libraries (nothing on Windows).
target_link_libraries(mpo_hle PRIVATE ${CMAKE_DL_LIBS})
if(MSVC)
    # C4324: CpuBridge keeps PSPRecomp's alignas(16) AllegrexContext per thread.
    target_compile_options(mpo_hle PUBLIC /wd4324)
endif()
mpo_target_warnings(mpo_hle)

# An executable that loads overlay libraries exports the runtime symbols they
# resolve against it (ELF: -rdynamic; Windows: every symbol in its import
# library, which the libraries link). Yakumo does the same.
function(mpo_enable_overlay_host target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS TRUE WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endfunction()

# One stage overlay library (F13): the generated sources of one corpus plus
# the entry point configured from overlay_module.cpp.in. `host` is the
# executable it resolves its symbols against; the remaining arguments are the
# fields of overlay.txt (generate.py --stage) and the sources.
#
#   mpo_add_overlay_library(<target> HOST <exe> OUTPUT_DIR <dir> PREFIX <p>
#       NAME <n> BASE <hex> SIZE <hex> PRX_SHA256 <h> TEXT_OFFSET <hex>
#       TEXT_SIZE <hex> TEXT_SHA256 <h> [GENERATED] SOURCES <files...>)
function(mpo_add_overlay_library target)
    cmake_parse_arguments(MPO_OVERLAY "GENERATED"
        "HOST;OUTPUT_DIR;PREFIX;NAME;BASE;SIZE;PRX_SHA256;TEXT_OFFSET;TEXT_SIZE;TEXT_SHA256"
        "SOURCES" ${ARGN})
    set(entry_point "${CMAKE_CURRENT_BINARY_DIR}/overlays/${MPO_OVERLAY_PREFIX}_module.cpp")
    configure_file("${MPO_HLE_SOURCE_DIR}/overlay_module.cpp.in" "${entry_point}" @ONLY)
    add_library(${target} MODULE ${MPO_OVERLAY_SOURCES} "${entry_point}")
    # Headers only: linking psprecomp_core would give the library its own copy
    # of the runtime state the executable owns.
    target_include_directories(${target} PRIVATE "${MPO_HLE_SOURCE_DIR}/..")
    target_include_directories(${target} SYSTEM PRIVATE
        $<TARGET_PROPERTY:psprecomp_core,INTERFACE_INCLUDE_DIRECTORIES>)
    if(WIN32 OR APPLE)
        # MSVC links the executable's import library; ld64 wants it as the
        # bundle loader. ELF leaves the symbols to the dynamic loader.
        target_link_libraries(${target} PRIVATE ${MPO_OVERLAY_HOST})
    else()
        add_dependencies(${target} ${MPO_OVERLAY_HOST})
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)
    endif()
    if(MPO_OVERLAY_GENERATED)
        # The rules of the mgp_main corpus (recomp/CMakeLists.txt).
        set_target_properties(${target} PROPERTIES JOB_POOL_COMPILE psprecomp_generated)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W0 /bigobj)
        else()
            target_compile_options(${target} PRIVATE -w -ffp-contract=off -fno-sanitize=all
                                   -O${PSPRECOMP_GENERATED_OPT_LEVEL})
        endif()
    endif()
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        OUTPUT_NAME "${MPO_OVERLAY_PREFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${MPO_OVERLAY_OUTPUT_DIR}")
    # Multi-config generators (MSVC) would add a per-config subdirectory.
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${MPO_OVERLAY_OUTPUT_DIR}")
    endforeach()
endfunction()
set(MPO_HLE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# The resident Konami modules, recompiled from the user's files like mgp_main
# (generate.py --module sound2|kjfs|zlibdec writes <dir>/<module>/generated).
set(MPO_GENERATED_MODULES_DIR "" CACHE PATH
    "generate.py output root with <module>/generated for sound2, kjfs and zlibdec; empty: not built")
# The stage overlays (F13): build_overlays.py output, <dir>/<s>/overlay.txt and
# <dir>/<s>/generated. Each one becomes a library in <build>/overlays that
# mpo_boot loads (--overlays, that directory by default).
set(MPO_GENERATED_OVERLAYS_DIR "" CACHE PATH
    "build_overlays.py output root with <stage>/overlay.txt; empty: no stage overlay is built")
set(MPO_OVERLAY_LIBRARY_DIR "${CMAKE_BINARY_DIR}/overlays")

# mpo_boot: boots mgp_main on the kernel and traces it (needs the corpus).
if(TARGET mpo_generated_mgp_main)
    include(${PROJECT_SOURCE_DIR}/third_party/ppsspp_cpu/ppsspp_vfpu_tables.cmake)
    add_executable(mpo_boot boot_main.cpp)
    target_link_libraries(mpo_boot PRIVATE psprecomp_core mpo_hle mpo_recomp mpo_generated_mgp_main
                          mpo_ge_replay mpo_savedata)
    # F10b: --window shows the display and reads the controls with SDL.
    if(TARGET mpo_frontend_sdl)
        target_link_libraries(mpo_boot PRIVATE mpo_frontend_sdl)
        target_compile_definitions(mpo_boot PRIVATE MPO_BOOT_SDL=1)
    endif()
    # F14b: --renderer vulkan, and with SDL a Vulkan swapchain for --window.
    if(TARGET mpo_vulkan)
        target_compile_definitions(mpo_boot PRIVATE MPO_BOOT_VULKAN=1)
        if(TARGET mpo_vulkan_sdl)
            target_link_libraries(mpo_boot PRIVATE mpo_vulkan_sdl)
        else()
            target_link_libraries(mpo_boot PRIVATE mpo_vulkan)
        endif()
    endif()
    target_compile_definitions(mpo_boot PRIVATE MPO_DEFAULT_VFPU_TABLES="${MPO_VFPU_TABLES_DIR}"
                               MPO_DEFAULT_OVERLAYS="${MPO_OVERLAY_LIBRARY_DIR}")
    mpo_target_warnings(mpo_boot)
    mpo_enable_overlay_host(mpo_boot)
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

    set(mpo_overlay_count 0)
    if(MPO_GENERATED_OVERLAYS_DIR)
        file(GLOB mpo_overlay_metadata CONFIGURE_DEPENDS "${MPO_GENERATED_OVERLAYS_DIR}/*/overlay.txt")
    else()
        set(mpo_overlay_metadata "")
    endif()
    foreach(meta IN LISTS mpo_overlay_metadata)
        get_filename_component(mpo_overlay_dir "${meta}" DIRECTORY)
        set(mpo_fields NAME PREFIX BASE SIZE PRX_SHA256 TEXT_OFFSET TEXT_SIZE TEXT_SHA256)
        foreach(field IN LISTS mpo_fields)
            unset(mpo_overlay_${field})
        endforeach()
        file(STRINGS "${meta}" mpo_meta_lines REGEX "^[a-z0-9_]+=.+$")
        foreach(line IN LISTS mpo_meta_lines)
            string(REGEX MATCH "^([a-z0-9_]+)=(.+)$" matched "${line}")
            string(TOUPPER "${CMAKE_MATCH_1}" key)
            set(mpo_overlay_${key} "${CMAKE_MATCH_2}")
        endforeach()
        set(mpo_overlay_args "")
        set(mpo_overlay_complete TRUE)
        foreach(field IN LISTS mpo_fields)
            if(NOT DEFINED mpo_overlay_${field})
                set(mpo_overlay_complete FALSE)
            endif()
            list(APPEND mpo_overlay_args ${field} "${mpo_overlay_${field}}")
        endforeach()
        file(GLOB mpo_overlay_sources CONFIGURE_DEPENDS "${mpo_overlay_dir}/generated/*.cpp")
        if(NOT mpo_overlay_complete OR NOT mpo_overlay_sources)
            message(WARNING "stage overlay: incomplete ${meta} or no generated sources; skipped")
            continue()
        endif()
        mpo_add_overlay_library(mpo_overlay_${mpo_overlay_NAME} HOST mpo_boot GENERATED
            OUTPUT_DIR "${MPO_OVERLAY_LIBRARY_DIR}" ${mpo_overlay_args} SOURCES ${mpo_overlay_sources})
        target_include_directories(mpo_overlay_${mpo_overlay_NAME} PRIVATE "${mpo_overlay_dir}/generated")
        math(EXPR mpo_overlay_count "${mpo_overlay_count} + 1")
    endforeach()
    message(STATUS "stage overlays: ${mpo_overlay_count} libraries in ${MPO_OVERLAY_LIBRARY_DIR}")
endif()
