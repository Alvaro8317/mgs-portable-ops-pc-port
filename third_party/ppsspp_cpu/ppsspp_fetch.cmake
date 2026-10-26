# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_fetch_ppsspp_files(<out_dir_var> <subdir> <relative> <sha256> [<relative> <sha256> ...])
#
# Copies unmodified files of PPSSPP v1.20.4 into <build>/_deps/<subdir>/, from
# MPO_PPSSPP_SOURCE_DIR when it is set or from raw.githubusercontent.com
# otherwise, and checks the SHA-256 of each one. Sets <out_dir_var> to the
# destination directory. Any mismatch or download failure is fatal.

set(MPO_PPSSPP_SOURCE_DIR "" CACHE PATH
    "Local PPSSPP checkout to take the pinned files from instead of downloading them")

set(MPO_PPSSPP_COMMIT "fa50bb1976065c4f8b1b47af227d367fe9771555") # tag v1.20.4

function(mpo_fetch_ppsspp_files out_dir_var subdir)
    set(files ${ARGN})
    set(destination_dir "${CMAKE_BINARY_DIR}/_deps/${subdir}")
    list(LENGTH files count)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last} 2)
        math(EXPR hash_index "${index} + 1")
        list(GET files ${index} relative)
        list(GET files ${hash_index} expected)
        set(destination "${destination_dir}/${relative}")
        if(EXISTS "${destination}")
            file(SHA256 "${destination}" actual)
            if(actual STREQUAL expected)
                continue()
            endif()
            file(REMOVE "${destination}")
        endif()
        if(MPO_PPSSPP_SOURCE_DIR)
            set(source "${MPO_PPSSPP_SOURCE_DIR}/${relative}")
            if(NOT EXISTS "${source}")
                message(FATAL_ERROR "MPO_PPSSPP_SOURCE_DIR lacks ${relative}")
            endif()
            file(SHA256 "${source}" actual)
            if(NOT actual STREQUAL expected)
                message(FATAL_ERROR "${source}: SHA-256 ${actual}, expected ${expected} "
                                    "(PPSSPP ${MPO_PPSSPP_COMMIT})")
            endif()
            configure_file("${source}" "${destination}" COPYONLY)
        else()
            set(url "https://raw.githubusercontent.com/hrydgard/ppsspp/${MPO_PPSSPP_COMMIT}/${relative}")
            file(DOWNLOAD "${url}" "${destination}" EXPECTED_HASH SHA256=${expected}
                 TLS_VERIFY ON STATUS status)
            list(GET status 0 status_code)
            if(NOT status_code EQUAL 0)
                file(REMOVE "${destination}")
                list(GET status 1 status_message)
                message(FATAL_ERROR "Cannot download ${url}: ${status_message}\n"
                                    "Build offline with -DMPO_PPSSPP_SOURCE_DIR=<ppsspp checkout>.")
            endif()
        endif()
    endforeach()
    set(${out_dir_var} "${destination_dir}" PARENT_SCOPE)
endfunction()
