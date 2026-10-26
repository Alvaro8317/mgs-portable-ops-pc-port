# SPDX-License-Identifier: GPL-3.0-or-later
#
# mpo_extract_mips_tables(<input MIPSTables.cpp> <output .cpp>)
#
# Builds a reduced copy of PPSSPP's Core/MIPS/MIPSTables.cpp with only the
# decode tables and MIPSGetInstruction(), so PPSSPP's own interpreter can be
# dispatched without its JIT, disassembler, debugger and core timing. The
# compile and disassembly columns of the tables are dropped by redefining the
# INSTR macro; every interpreter pointer is kept as is. A failed substitution
# is fatal, so a different upstream file can never be half-converted.

function(mpo_extract_mips_tables input output)
    file(READ "${input}" text)

    string(FIND "${text}" "enum MipsEncoding {" begin)
    string(FIND "${text}" "void MIPSCompileOp(" end)
    if(begin EQUAL -1 OR end EQUAL -1 OR end LESS begin)
        message(FATAL_ERROR "${input}: decode tables not found")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${text}" ${begin} ${length} body)

    # Each substitution must match exactly once, or the upstream file is not
    # the pinned one.
    macro(_mpo_replace from to)
        string(FIND "${body}" "${from}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${input}: expected text not found: ${from}")
        endif()
        string(REPLACE "${from}" "${to}" body "${body}")
    endmacro()
    _mpo_replace("MIPSComp::MIPSCompileFunc compile" "const void *compile")
    _mpo_replace("MIPSDisFunc disasm" "const void *disasm")
    _mpo_replace("{Instruc, name, comp, dis, inter, MIPSInfo(flags)}"
                 "{Instruc, name, nullptr, nullptr, inter, MIPSInfo(flags)}")
    _mpo_replace("#define JITFUNC(f) (&MIPSFrontendInterface::f)" "#define JITFUNC(f) nullptr")

    set(prologue [=[
// Generated at configure time by third_party/ppsspp_cpu/extract_mips_tables.cmake
// from PPSSPP Core/MIPS/MIPSTables.cpp @ v1.20.4 (GPL-2.0-or-later). Do not edit.

#include "Common/CommonTypes.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSInt.h"
#include "Core/MIPS/MIPSIntVFPU.h"
#include "Core/MIPS/MIPSTables.h"

namespace MIPSDis {}
namespace MIPSComp {}

]=])
    set(epilogue [=[

MIPSInfo MIPSGetInfo(MIPSOpcode op) {
	const MIPSInstruction *instr = MIPSGetInstruction(op);
	return instr ? instr->flags : MIPSInfo(BAD_INSTRUCTION);
}

// Set when the last MIPSInterpret() found no interpreter for the opcode.
bool g_mpo_ref_unsupported = false;

void MIPSInterpret(MIPSOpcode op) {
	const MIPSInstruction *instr = MIPSGetInstruction(op);
	if (instr && instr->interpret) {
		instr->interpret(op);
	} else {
		g_mpo_ref_unsupported = true;
	}
}
]=])
    file(WRITE "${output}.tmp" "${prologue}${body}${epilogue}")
    file(COPY_FILE "${output}.tmp" "${output}" ONLY_IF_DIFFERENT)
    file(REMOVE "${output}.tmp")
endfunction()
