# SPDX-License-Identifier: GPL-3.0-or-later
#
# PSP module decryption for the installer: libkirk and Core/ELF/PrxDecrypter.cpp
# from PPSSPP, fetched unmodified at configure time and verified by SHA-256.
#
# These files embed the KIRK keyvault and the PRX tag key tables, so they are
# never committed (CLAUDE.md §1.1). They are compiled only into the targets
# that link mpo_prx_decrypter (the installer). See NOTICE.md.
#
# Defines, when MPO_ENABLE_PRX_DECRYPT is ON:
#   mpo_prx_decrypter  static library exporting pspDecryptPRX()

option(MPO_ENABLE_PRX_DECRYPT
       "Build EBOOT decryption into the installer (fetches libkirk and PrxDecrypter from PPSSPP)"
       ON)
set(MPO_PPSSPP_SOURCE_DIR "" CACHE PATH
    "Local PPSSPP checkout to take the pinned files from instead of downloading them")

if(NOT MPO_ENABLE_PRX_DECRYPT)
    return()
endif()

set(mpo_ppsspp_commit "fa50bb1976065c4f8b1b47af227d367fe9771555") # tag v1.20.4
set(mpo_ppsspp_files
    ext/libkirk/AES.c         3e0321521c1985aceac86d0b5388ab5256a58b94b82092ef48c658756b19d09f
    ext/libkirk/AES.h         61e784ebd7ddcb6f7876f1933c1e2020f7fc89ab3a1f3bfcb50c29cd35abf179
    ext/libkirk/SHA1.c        7ef8ee993a7858ff78fdf22efb8c2e06b4864513f6842396b1cf7035f7604024
    ext/libkirk/SHA1.h        527c0407568beea5b3a07675ec1e25cf477208cd68ba2e45d462fdcb1cd21237
    ext/libkirk/bn.c          06d2886369f2490855e9e1541ab84901b4a5b03661fbbcd6899444d18c28718f
    ext/libkirk/ec.c          f86bc1c13680da95486538b0202467e089a2f50907caebe1afa31d489d96df48
    ext/libkirk/kirk_common.h 8bc730e60c1a5498cec2f649bf8bc356ce85a843da8d2f4a278f19d14cce17a5
    ext/libkirk/kirk_engine.c 74e363f78fdec5c0fc82842a21c11c3b0363996a931805cec9fd11231a3e7763
    ext/libkirk/kirk_engine.h d827ae841fe160a52ae4ad8f118c6d46476a5a0887d349328419197ad5e996bc
    Core/ELF/PrxDecrypter.cpp 9bea13a3867b7b7e60b26f648bfcf650ce96e9f0da2203949cf1f9c2df54659b
)
set(mpo_ppsspp_dir "${CMAKE_BINARY_DIR}/_deps/ppsspp_crypto")

list(LENGTH mpo_ppsspp_files mpo_ppsspp_count)
math(EXPR mpo_ppsspp_last "${mpo_ppsspp_count} - 1")
foreach(index RANGE 0 ${mpo_ppsspp_last} 2)
    math(EXPR hash_index "${index} + 1")
    list(GET mpo_ppsspp_files ${index} relative)
    list(GET mpo_ppsspp_files ${hash_index} expected)
    set(destination "${mpo_ppsspp_dir}/${relative}")
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
                                "(PPSSPP ${mpo_ppsspp_commit})")
        endif()
        configure_file("${source}" "${destination}" COPYONLY)
    else()
        set(url "https://raw.githubusercontent.com/hrydgard/ppsspp/${mpo_ppsspp_commit}/${relative}")
        file(DOWNLOAD "${url}" "${destination}" EXPECTED_HASH SHA256=${expected}
             TLS_VERIFY ON STATUS status)
        list(GET status 0 status_code)
        if(NOT status_code EQUAL 0)
            file(REMOVE "${destination}")
            list(GET status 1 status_message)
            message(FATAL_ERROR "Cannot download ${url}: ${status_message}\n"
                                "Build offline with -DMPO_PPSSPP_SOURCE_DIR=<ppsspp checkout> "
                                "or disable decryption with -DMPO_ENABLE_PRX_DECRYPT=OFF.")
        endif()
    endif()
endforeach()

enable_language(C)

# Upstream code: built with its own (default) warnings, never with project
# flags or -Werror.
add_library(mpo_kirk STATIC
    "${mpo_ppsspp_dir}/ext/libkirk/AES.c"
    "${mpo_ppsspp_dir}/ext/libkirk/SHA1.c"
    "${mpo_ppsspp_dir}/ext/libkirk/bn.c"
    "${mpo_ppsspp_dir}/ext/libkirk/ec.c"
    "${mpo_ppsspp_dir}/ext/libkirk/kirk_engine.c"
)
target_include_directories(mpo_kirk SYSTEM PUBLIC "${mpo_ppsspp_dir}")
if(MSVC)
    target_compile_definitions(mpo_kirk PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mpo_kirk PRIVATE /w)
else()
    target_compile_options(mpo_kirk PRIVATE -w)
endif()

# The shim directory replaces the PPSSPP headers PrxDecrypter.cpp includes
# (Common/*.h and Core/ELF/PrxDecrypter.h); it must come first.
add_library(mpo_prx_decrypter STATIC "${mpo_ppsspp_dir}/Core/ELF/PrxDecrypter.cpp")
target_include_directories(mpo_prx_decrypter BEFORE PUBLIC
    "${PROJECT_SOURCE_DIR}/profiles/mpo/host/crypto/ppsspp_shim")
target_link_libraries(mpo_prx_decrypter PUBLIC mpo_kirk)
if(MSVC)
    target_compile_options(mpo_prx_decrypter PRIVATE /w)
else()
    target_compile_options(mpo_prx_decrypter PRIVATE -w)
endif()
