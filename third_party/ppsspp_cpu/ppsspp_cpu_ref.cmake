# SPDX-License-Identifier: GPL-3.0-or-later
#
# PPSSPP's interpreter sources, fetched unmodified at configure time for the
# test-only reference executor and golden generator (mpo_ppsspp_ref). They are
# never linked into the runtime. See NOTICE.md.
#
# Defines, when MPO_BUILD_PPSSPP_REF is ON:
#   MPO_PPSSPP_CPU_DIR     root of the fetched files (include directory)
#   MPO_PPSSPP_CPU_SOURCES the .cpp files to compile

include(${CMAKE_CURRENT_LIST_DIR}/ppsspp_fetch.cmake)

# Off by default with MSVC: the reference relies on -ftrivial-auto-var-init=zero
# (GCC/Clang) to read upstream's uninitialised lanes as zero.
if(MSVC)
    set(mpo_ppsspp_ref_default OFF)
else()
    set(mpo_ppsspp_ref_default ON)
endif()
option(MPO_BUILD_PPSSPP_REF
       "Build PPSSPP's interpreter as a test-only difftest reference and golden generator"
       ${mpo_ppsspp_ref_default})

set(MPO_PPSSPP_CPU_DIR "")
set(MPO_PPSSPP_CPU_SOURCES "")
if(MPO_BUILD_PPSSPP_REF)
    mpo_fetch_ppsspp_files(MPO_PPSSPP_CPU_DIR ppsspp_cpu
        ppsspp_config.h                         50622a3dd1c665543257f701708c61a81aa02f0c9618edc28ff54d1504e77c87
        Common/BitScan.h                        1f8d873477c3f0317b14437c5b949ee4987ca073d41ac2033b6cd0e8c0663d32
        Common/BitSet.h                         efd85b0a1e888a6f17aeae9020360ab1ce6b3d847e24bec32c0183814ef6a916
        Common/Common.h                         cd56ac49b415a4f0c785f3494d4859d0c91ce2de1fc7b18fb04f616fd36e90c6
        Common/CommonFuncs.h                    82088be43750414fa70974c851c3d621629a3dc74762d484435a68ad38f81ff4
        Common/CommonTypes.h                    5cde07034c9d02d2739e8c40801096f409b14b35de68d395d35640af75e97cae
        Common/CommonWindows.h                  d5d228c3768fc83538aa43b02846fb0b938d60052bf2b257e8d4d71f0b3feba6
        Common/Data/Convert/SmallDataConvert.h  172ffa06d88980ac33e3d6f7a9f091de55ddd2df3fcc85684e280bbfbbfa94a7
        Common/Math/SIMDHeaders.h               caf8b5f31f24edf82cb93ddfb92951c66499001accd736c2e355437a3dece319
        Common/Math/math_util.h                 6fbb9aa5d4c90aaa61b1b15b6921224eca4e4b1ea44c9cab77890bf02f112370
        Core/Opcode.h                           70b5e6fb4caa7ba6752c9e4a11fd54a3bb89b892e026ce688d551668fa8f55fc
        Core/MIPS/MIPS.h                        1011e22125e9b5b6a1e1b73228736fa39622ceb50ba7e3de4e15ec315811529c
        Core/MIPS/MIPSCodeUtils.h               d48376314e8f8f51d19da300007a33f93a4a8218e393baf9809c82586afa2de9
        Core/MIPS/MIPSInt.cpp                   8aa885708c30cc125a9a553b703317171ee657b2f6d8144c5797d401e5847b95
        Core/MIPS/MIPSInt.h                     c7ed00a492053b48f044b6eefbd186b73ffae0a0f1ef2ccdee88151b43fa825a
        Core/MIPS/MIPSIntVFPU.cpp               9a5f53df3b5f8c0a1b48424eae8430c729a89b89fab1d8926e553226dfcab876
        Core/MIPS/MIPSIntVFPU.h                 34575b82b1bea49f5d37cd70fc48cfec5e4aaaa4289e25d73e9abd90e7292899
        Core/MIPS/MIPSTables.cpp                960cdfcb001780ff8eb629eb8d0d1cd911dd96c3aa654bc3ad6c2860b5cca637
        Core/MIPS/MIPSTables.h                  1a50be449a053e91378543b49d40ca0c2c540fa3534cb127b62094c0843b788d
        Core/MIPS/MIPSVFPUFallbacks.cpp         77068a5ae3d5a4bbc15f395462dd56b3bcf0392342ea60e3eebca982bd131cca
        Core/MIPS/MIPSVFPUFallbacks.h           84c78b036aa027d2497f4a8bbce38661e9e3a44ffce4e0912e9f9f2231475cee
        Core/MIPS/MIPSVFPUUtils.cpp             33c419ce793f5fc2db2cd825626541d823ce975c81784d67cd87fee53d115525
        Core/MIPS/MIPSVFPUUtils.h               262d77fcc9dda7d63d56ff12cf93338d86bd4373f8fa57eb073c55a6b6cfc7e9
    )
    # MIPSTables.cpp drags in the JIT, the disassembler and the debugger; only
    # its decode tables are kept (see extract_mips_tables.cmake).
    include(${CMAKE_CURRENT_LIST_DIR}/extract_mips_tables.cmake)
    set(mpo_tables_cpp "${CMAKE_BINARY_DIR}/_deps/ppsspp_cpu_generated/MIPSTablesDecode.cpp")
    mpo_extract_mips_tables("${MPO_PPSSPP_CPU_DIR}/Core/MIPS/MIPSTables.cpp" "${mpo_tables_cpp}")
    set(MPO_PPSSPP_CPU_SOURCES
        "${MPO_PPSSPP_CPU_DIR}/Core/MIPS/MIPSInt.cpp"
        "${MPO_PPSSPP_CPU_DIR}/Core/MIPS/MIPSIntVFPU.cpp"
        "${MPO_PPSSPP_CPU_DIR}/Core/MIPS/MIPSVFPUUtils.cpp"
        "${MPO_PPSSPP_CPU_DIR}/Core/MIPS/MIPSVFPUFallbacks.cpp"
        "${mpo_tables_cpp}"
    )
endif()
