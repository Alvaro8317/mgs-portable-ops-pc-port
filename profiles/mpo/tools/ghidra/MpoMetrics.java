// SPDX-License-Identifier: GPL-3.0-or-later
// Measures the current program for the recompiler and writes
// <outDir>/<program>.metrics.json:
//   - blocks: name, range, flags of every memory block;
//   - coverage of every executable block: bytes decoded as instructions, bytes
//     inside function bodies, and undefined bytes (zero vs non-zero, with the
//     non-zero runs);
//   - instruction classes, decided from the opcode bits (not from Ghidra's
//     mnemonics, which carry a "_" prefix in delay slots): VFPU (and its
//     prefixes vpfxs/vpfxt/vpfxd), FPU, branch-likely, jr/jalr (resolved or
//     not), syscall/break, cache, ll/sc, Allegrex SPECIAL2/SPECIAL3;
//   - a mnemonic histogram and the number of functions that use the VFPU/FPU.
//
// "Resolved" means Ghidra has at least one non-fallthrough flow reference from
// the instruction (switch tables recovered by the Decompiler Switch Analysis or
// a constant register target). `jr ra` is counted apart as a return.
//
// Usage (headless): -postScript MpoMetrics.java <outDir>
//@category MPO

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

public class MpoMetrics extends GhidraScript {
    private static final Set<Integer> VFPU_OPCODES =
        Set.of(0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F);

    private final Map<String, Integer> classes = new TreeMap<>();
    private final Map<String, Integer> mnemonics = new TreeMap<>();
    private final Map<String, Integer> vfpuMnemonics = new TreeMap<>();
    private final List<Object> unresolvedJr = new ArrayList<>();
    private int unresolvedJalr;

    private void count(String key) {
        classes.merge(key, 1, Integer::sum);
    }

    private boolean resolved(Instruction ins) {
        for (Reference r : getReferencesFrom(ins.getAddress())) {
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isFallthrough()) {
                return true;
            }
        }
        return false;
    }

    /** Counts the classes of one instruction word; returns "vfpu", "fpu" or "". */
    private String classify(Instruction ins, int word) {
        int op = word >>> 26;
        int rs = (word >>> 21) & 0x1F;
        int rt = (word >>> 16) & 0x1F;
        int funct = word & 0x3F;
        if (VFPU_OPCODES.contains(op)) {
            count("vfpu");
            if (op == 0x37) {
                int sub = (word >>> 24) & 0x3;
                count(sub == 0 ? "vfpu.prefix.vpfxs" : sub == 1 ? "vfpu.prefix.vpfxt"
                    : sub == 2 ? "vfpu.prefix.vpfxd" : "vfpu.viim_vfim");
            }
            if (op == 0x12 && rs == 0x08) {
                count((rt & 2) != 0 ? "branch_likely.bvfl_bvtl" : "branch.bvf_bvt");
            }
            if (op == 0x32 || op == 0x35 || op == 0x36 || op == 0x3A || op == 0x3D || op == 0x3E) {
                count("vfpu.load_store");
            }
            return "vfpu";
        }
        switch (op) {
            case 0x00 -> {
                if (funct == 0x08) {
                    if (rs == 31) {
                        count("jr.ra");
                    } else if (resolved(ins)) {
                        count("jr.resolved");
                    } else {
                        count("jr.unresolved");
                        Function f = getFunctionContaining(ins.getAddress());
                        Map<String, Object> e = new LinkedHashMap<>();
                        e.put("at", String.format("0x%08X", ins.getAddress().getOffset()));
                        e.put("function", f == null ? null : String.format("0x%08X", f.getEntryPoint().getOffset()));
                        unresolvedJr.add(e);
                    }
                } else if (funct == 0x09) {
                    if (resolved(ins)) {
                        count("jalr.resolved");
                    } else {
                        count("jalr.unresolved");
                        unresolvedJalr++;
                    }
                } else if (funct == 0x0C) {
                    count("syscall");
                } else if (funct == 0x0D) {
                    count("break");
                } else if (funct == 0x0F) {
                    count("sync");
                } else if (funct == 0x0A || funct == 0x0B) {
                    count("allegrex.movz_movn");
                } else if (funct == 0x2C || funct == 0x2D) {
                    count("allegrex.max_min");
                } else if (funct == 0x16 || funct == 0x17) {
                    count("allegrex.clz_clo");
                } else if (funct == 0x1C || funct == 0x1D || funct == 0x2E || funct == 0x2F) {
                    count("allegrex.madd_msub");
                }
            }
            case 0x01 -> {
                if (rt == 0x02 || rt == 0x03 || rt == 0x12 || rt == 0x13) {
                    count("branch_likely.regimm");
                }
            }
            case 0x14, 0x15, 0x16, 0x17 -> count("branch_likely.beql_bnel_blezl_bgtzl");
            case 0x11 -> {
                count("fpu");
                if (rs == 0x08 && (rt & 2) != 0) {
                    count("branch_likely.bc1fl_bc1tl");
                }
                return "fpu";
            }
            case 0x31, 0x39 -> {
                count("fpu");
                count("fpu.load_store");
                return "fpu";
            }
            case 0x10 -> count("cop0");
            case 0x1C -> count("allegrex.special2");
            case 0x1F -> count("allegrex.special3");
            case 0x2F -> count("cache");
            case 0x30, 0x38 -> count("ll_sc");
            case 0x22, 0x26, 0x2A, 0x2E -> count("lwl_lwr_swl_swr");
            default -> {
            }
        }
        return "";
    }

    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        Files.createDirectories(outDir);
        Memory mem = currentProgram.getMemory();

        List<Object> blocks = new ArrayList<>();
        for (MemoryBlock b : mem.getBlocks()) {
            Map<String, Object> e = new LinkedHashMap<>();
            e.put("name", b.getName());
            e.put("start", String.format("0x%08X", b.getStart().getOffset()));
            e.put("size", b.getSize());
            e.put("flags", (b.isRead() ? "r" : "-") + (b.isWrite() ? "w" : "-") + (b.isExecute() ? "x" : "-")
                + (b.isInitialized() ? "" : " uninit"));
            blocks.add(e);
        }

        List<Object> coverage = new ArrayList<>();
        Set<Function> vfpuFunctions = new HashSet<>();
        Set<Function> fpuFunctions = new HashSet<>();
        int totalInstructions = 0;
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isExecute() || !b.isInitialized()) {
                continue;
            }
            AddressSet range = new AddressSet(b.getStart(), b.getEnd());
            long size = b.getSize();
            long instrBytes = 0;
            int instrCount = 0;
            for (Instruction ins : currentProgram.getListing().getInstructions(range, true)) {
                monitor.checkCancelled();
                instrBytes += ins.getLength();
                instrCount++;
                int word = mem.getInt(ins.getAddress());
                String kind = classify(ins, word);
                String m = ins.getMnemonicString().replaceFirst("^_", "");
                mnemonics.merge(m, 1, Integer::sum);
                Function f = getFunctionContaining(ins.getAddress());
                if (kind.equals("vfpu")) {
                    vfpuMnemonics.merge(m, 1, Integer::sum);
                    if (f != null) {
                        vfpuFunctions.add(f);
                    }
                } else if (kind.equals("fpu") && f != null) {
                    fpuFunctions.add(f);
                }
            }
            totalInstructions += instrCount;

            AddressSet inFunctions = new AddressSet();
            int functionCount = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(range, true)) {
                functionCount++;
                inFunctions.add(f.getBody().intersect(range));
            }
            AddressSetView undefined = range.subtract(instructionSet(range));
            long zeroBytes = 0;
            long nonZeroBytes = 0;
            List<Object> nonZeroRuns = new ArrayList<>();
            for (AddressRange r : undefined) {
                long nz = 0;
                for (Address a = r.getMinAddress(); a.compareTo(r.getMaxAddress()) <= 0; a = a.add(1)) {
                    if (mem.getByte(a) != 0) {
                        nz++;
                    }
                    if (a.equals(r.getMaxAddress())) {
                        break;
                    }
                }
                zeroBytes += r.getLength() - nz;
                nonZeroBytes += nz;
                if (nz > 0) {
                    Function before = getFunctionBefore(r.getMinAddress());
                    Map<String, Object> run = new LinkedHashMap<>();
                    run.put("start", String.format("0x%08X", r.getMinAddress().getOffset()));
                    run.put("length", r.getLength());
                    run.put("non_zero", nz);
                    run.put("after_function", before == null ? null
                        : String.format("0x%08X", before.getEntryPoint().getOffset()));
                    nonZeroRuns.add(run);
                }
            }
            Map<String, Object> e = new LinkedHashMap<>();
            e.put("block", b.getName());
            e.put("size", size);
            e.put("instructions", instrCount);
            e.put("instruction_bytes", instrBytes);
            e.put("function_bytes", inFunctions.getNumAddresses());
            e.put("functions", functionCount);
            e.put("undefined_zero_bytes", zeroBytes);
            e.put("undefined_nonzero_bytes", nonZeroBytes);
            e.put("decoded_percent", 100.0 * instrBytes / size);
            e.put("decoded_percent_excluding_zero_padding", 100.0 * instrBytes / (size - zeroBytes));
            e.put("function_percent", 100.0 * inFunctions.getNumAddresses() / size);
            e.put("nonzero_undefined_runs", nonZeroRuns);
            coverage.add(e);
        }

        int functions = currentProgram.getFunctionManager().getFunctionCount();
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("program", currentProgram.getName());
        out.put("image_base", String.format("0x%08X", currentProgram.getImageBase().getOffset()));
        out.put("functions_total", functions);
        out.put("functions_with_vfpu", vfpuFunctions.size());
        out.put("functions_with_fpu", fpuFunctions.size());
        out.put("instructions", totalInstructions);
        out.put("classes", classes);
        out.put("unresolved_jr", unresolvedJr);
        out.put("blocks", blocks);
        out.put("coverage", coverage);
        out.put("vfpu_mnemonics", vfpuMnemonics);
        out.put("mnemonics", mnemonics);
        Files.writeString(outDir.resolve(currentProgram.getName() + ".metrics.json"), MpoJson.value(out),
            StandardCharsets.UTF_8);
        println("METRICS " + currentProgram.getName() + ": " + functions + " functions, " + totalInstructions
            + " instructions, " + classes);
    }

    private AddressSet instructionSet(AddressSet range) {
        AddressSet set = new AddressSet();
        for (Instruction ins : currentProgram.getListing().getInstructions(range, true)) {
            set.add(ins.getMinAddress(), ins.getMaxAddress());
        }
        return set;
    }
}
