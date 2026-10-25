// SPDX-License-Identifier: GPL-3.0-or-later
// Exports what the recompiler needs from the Ghidra analysis of the current
// program to <outDir>/<program>.recomp.json:
//   - image_base and the executable blocks;
//   - functions: entry, name, name source, body ranges, and the library:NID
//     when it is an import stub (from the .lib.stub table);
//   - jump_tables: every `jr <reg>` other than `jr ra`, with its flow targets
//     (empty when Ghidra could not resolve it);
//   - indirect_calls: every `jalr` that Ghidra resolved, with its targets.
// Same idea as gow-psp-recomp's ExportRecompInput.java, written for this
// project. The output depends on the user's executable, so it stays in the
// ignored analysis/ directory.
//
// Usage (headless): -postScript MpoExportRecompInput.java <outDir>
//@category MPO

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

public class MpoExportRecompInput extends GhidraScript {
    private static String hex(long v) {
        return String.format("0x%08X", v);
    }

    private List<Object> flowTargets(Instruction ins) {
        TreeSet<Long> targets = new TreeSet<>();
        for (Reference r : getReferencesFrom(ins.getAddress())) {
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isFallthrough()) {
                targets.add(r.getToAddress().getOffset());
            }
        }
        List<Object> out = new ArrayList<>();
        for (long t : targets) {
            out.add(hex(t));
        }
        return out;
    }

    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        Files.createDirectories(outDir);
        Memory mem = currentProgram.getMemory();
        java.util.TreeMap<Long, String> stubs = MpoPsp.importStubs(currentProgram);

        AddressSet exec = new AddressSet();
        List<Object> blocks = new ArrayList<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (b.isExecute() && b.isInitialized()) {
                exec.add(b.getStart(), b.getEnd());
                Map<String, Object> e = new LinkedHashMap<>();
                e.put("name", b.getName());
                e.put("start", hex(b.getStart().getOffset()));
                e.put("end", hex(b.getEnd().getOffset() + 1));
                blocks.add(e);
            }
        }

        Path out = outDir.resolve(currentProgram.getName() + ".recomp.json");
        int functionCount = 0;
        int jumpTables = 0;
        int unresolved = 0;
        int calls = 0;
        try (BufferedWriter w = Files.newBufferedWriter(out, StandardCharsets.UTF_8)) {
            w.write("{\"program\":" + MpoJson.str(currentProgram.getName()));
            w.write(",\"image_base\":" + MpoJson.hex(currentProgram.getImageBase().getOffset()));
            w.write(",\"exec_blocks\":" + MpoJson.value(blocks));
            w.write(",\"functions\":[");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(exec, true)) {
                monitor.checkCancelled();
                List<Object> ranges = new ArrayList<>();
                for (AddressRange r : f.getBody()) {
                    ranges.add(List.of(hex(r.getMinAddress().getOffset()), hex(r.getMaxAddress().getOffset() + 1)));
                }
                Map<String, Object> e = new LinkedHashMap<>();
                e.put("addr", hex(f.getEntryPoint().getOffset()));
                e.put("name", f.getName());
                e.put("source", f.getSymbol().getSource().toString());
                e.put("import", stubs.get(f.getEntryPoint().getOffset()));
                e.put("ranges", ranges);
                w.write((first ? "" : ",") + MpoJson.value(e));
                first = false;
                functionCount++;
            }
            w.write("],\"jump_tables\":[");
            StringBuilder indirect = new StringBuilder();
            first = true;
            for (Instruction ins : currentProgram.getListing().getInstructions(exec, true)) {
                monitor.checkCancelled();
                int word = mem.getInt(ins.getAddress());
                if (word >>> 26 != 0) {
                    continue;
                }
                int funct = word & 0x3F;
                int rs = (word >>> 21) & 0x1F;
                if (funct == 0x08 && rs != 31) {
                    List<Object> targets = flowTargets(ins);
                    Function f = getFunctionContaining(ins.getAddress());
                    Map<String, Object> e = new LinkedHashMap<>();
                    e.put("at", hex(ins.getAddress().getOffset()));
                    e.put("function", f == null ? null : hex(f.getEntryPoint().getOffset()));
                    e.put("targets", targets);
                    w.write((first ? "" : ",") + MpoJson.value(e));
                    first = false;
                    jumpTables++;
                    if (targets.isEmpty()) {
                        unresolved++;
                    }
                } else if (funct == 0x09) {
                    List<Object> targets = flowTargets(ins);
                    if (targets.isEmpty()) {
                        continue;
                    }
                    Map<String, Object> e = new LinkedHashMap<>();
                    e.put("at", hex(ins.getAddress().getOffset()));
                    e.put("targets", targets);
                    indirect.append(indirect.length() == 0 ? "" : ",").append(MpoJson.value(e));
                    calls++;
                }
            }
            w.write("],\"indirect_calls\":[" + indirect + "]}");
        }
        println("RECOMP-INPUT " + currentProgram.getName() + ": " + functionCount + " functions, " + jumpTables
            + " jr (non-ra), " + unresolved + " unresolved, " + calls + " resolved jalr");
    }
}
