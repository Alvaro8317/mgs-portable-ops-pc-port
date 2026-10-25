// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the PSP import (.lib.stub) and export (.lib.ent) tables of the current
// program, names every import stub from a PPSSPP NID table, and writes
// <outDir>/<program>.modules.json with one entry per library: version,
// attributes, NIDs, resolved names, PPSSPP status and the number of call sites.
//
// The NID table is the TSV produced by profiles/mpo/tools/build_nid_table.py
// (nid, library, name, implemented, file). Lookup is by (library, NID) first and
// by NID alone as a fallback; the fallback is reported as "nid-only".
//
// Usage (headless): -postScript MpoModuleTables.java <nids.tsv> <outDir>
//@category MPO

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class MpoModuleTables extends GhidraScript {
    private record Nid(String library, String name, boolean implemented) {}

    private final Map<String, Nid> byLibraryAndNid = new HashMap<>();
    private final Map<Long, Nid> byNid = new HashMap<>();
    private final Map<Long, Integer> nidLibraryCount = new HashMap<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException("usage: MpoModuleTables.java <nids.tsv> <outDir>");
        }
        loadNids(Path.of(args[0]));
        Path outDir = Path.of(args[1]);
        Files.createDirectories(outDir);

        Map<String, Object> out = new LinkedHashMap<>();
        out.put("program", currentProgram.getName());
        out.put("image_base", String.format("0x%08X", currentProgram.getImageBase().getOffset()));
        out.put("nid_table", Path.of(args[0]).getFileName().toString());
        out.put("imports", readImports());
        out.put("exports", readExports());
        Files.writeString(outDir.resolve(currentProgram.getName() + ".modules.json"),
            MpoJson.value(out), StandardCharsets.UTF_8);
    }

    private void loadNids(Path tsv) throws Exception {
        for (String line : Files.readAllLines(tsv, StandardCharsets.UTF_8)) {
            String[] f = line.split("\t");
            if (f.length < 4) {
                continue;
            }
            long nid = Long.parseLong(f[0], 16);
            Nid entry = new Nid(f[1], f[2], f[3].equals("1"));
            byLibraryAndNid.put(f[1] + ":" + f[0].toUpperCase(), entry);
            if (!byNid.containsKey(nid) || entry.implemented() && !byNid.get(nid).implemented()) {
                byNid.put(nid, entry);
            }
            nidLibraryCount.merge(nid, 1, Integer::sum);
        }
    }

    private long u32(Address a) throws Exception {
        return currentProgram.getMemory().getInt(a) & 0xFFFFFFFFL;
    }

    private int u16(Address a) throws Exception {
        return currentProgram.getMemory().getShort(a) & 0xFFFF;
    }

    private int u8(Address a) throws Exception {
        return currentProgram.getMemory().getByte(a) & 0xFF;
    }

    private String cString(long offset) throws Exception {
        if (offset == 0) {
            return "";
        }
        Memory mem = currentProgram.getMemory();
        Address a = toAddr(offset);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 256; i++) {
            byte b = mem.getByte(a.add(i));
            if (b == 0) {
                break;
            }
            sb.append((char) (b & 0xFF));
        }
        return sb.toString();
    }

    private MemoryBlock requireBlock(String name) {
        MemoryBlock block = currentProgram.getMemory().getBlock(name);
        if (block == null) {
            throw new IllegalStateException("no " + name + " block in " + currentProgram.getName());
        }
        return block;
    }

    private List<Object> readImports() throws Exception {
        List<Object> libraries = new ArrayList<>();
        MemoryBlock stubs = currentProgram.getMemory().getBlock(".lib.stub");
        if (stubs == null) {
            println("MODULES " + currentProgram.getName() + ": no .lib.stub block, no imports");
            return libraries;
        }
        int total = 0;
        int named = 0;
        int implemented = 0;
        Address a = stubs.getStart();
        while (a.compareTo(stubs.getEnd()) < 0) {
            monitor.checkCancelled();
            int entLen = u8(a.add(8));
            if (entLen < 5) {
                throw new IllegalStateException("bad stub entry length " + entLen + " at " + a);
            }
            String lib = cString(u32(a));
            int version = u16(a.add(4));
            int attr = u16(a.add(6));
            int varCount = u8(a.add(9));
            int funcCount = u16(a.add(10));
            long nidPtr = u32(a.add(12));
            long stubPtr = u32(a.add(16));
            List<Object> functions = new ArrayList<>();
            for (int i = 0; i < funcCount; i++) {
                long nid = u32(toAddr(nidPtr + 4L * i));
                Address stub = toAddr(stubPtr + 8L * i);
                String key = lib + ":" + String.format("%08X", nid);
                Nid n = byLibraryAndNid.get(key);
                String match = "library";
                if (n == null) {
                    n = byNid.get(nid);
                    match = n == null ? "unknown" : "nid-only";
                }
                String name = n != null ? n.name() : String.format("%s_%08X", lib, nid);
                Function f = getFunctionAt(stub);
                if (f == null) {
                    f = createFunction(stub, name);
                }
                if (f != null) {
                    f.setName(name, SourceType.IMPORTED);
                }
                int callSites = 0;
                for (Reference r : getReferencesTo(stub)) {
                    if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) {
                        callSites++;
                    }
                }
                total++;
                if (n != null) {
                    named++;
                    if (n.implemented()) {
                        implemented++;
                    }
                }
                Map<String, Object> fn = new LinkedHashMap<>();
                fn.put("nid", String.format("0x%08X", nid));
                fn.put("name", name);
                fn.put("stub", String.format("0x%08X", stub.getOffset()));
                fn.put("call_sites", callSites);
                fn.put("match", match);
                fn.put("ppsspp", n == null ? "unknown" : n.implemented() ? "implemented" : "unimplemented");
                functions.add(fn);
            }
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("name", lib);
            entry.put("version", String.format("0x%04X", version));
            entry.put("attr", String.format("0x%04X", attr));
            entry.put("variables", varCount);
            entry.put("functions", functions);
            libraries.add(entry);
            a = a.add(entLen * 4L);
        }
        println("MODULES " + currentProgram.getName() + ": " + libraries.size() + " import libraries, "
            + total + " functions, " + named + " named, " + implemented + " implemented in PPSSPP");
        return libraries;
    }

    private List<Object> readExports() throws Exception {
        List<Object> libraries = new ArrayList<>();
        MemoryBlock ents = currentProgram.getMemory().getBlock(".lib.ent");
        if (ents == null) {
            return libraries;
        }
        Address a = ents.getStart();
        while (a.compareTo(ents.getEnd()) < 0) {
            monitor.checkCancelled();
            int entLen = u8(a.add(8));
            if (entLen < 4) {
                throw new IllegalStateException("bad export entry length " + entLen + " at " + a);
            }
            String lib = cString(u32(a));
            int version = u16(a.add(4));
            int attr = u16(a.add(6));
            int varCount = u8(a.add(9));
            int funcCount = u16(a.add(10));
            long table = u32(a.add(12));
            int count = varCount + funcCount;
            List<Object> entries = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                long nid = u32(toAddr(table + 4L * i));
                long target = u32(toAddr(table + 4L * (count + i)));
                Nid n = byNid.get(nid);
                Map<String, Object> e = new LinkedHashMap<>();
                e.put("kind", i < funcCount ? "function" : "variable");
                e.put("nid", String.format("0x%08X", nid));
                e.put("name", n == null ? null : n.name());
                e.put("address", String.format("0x%08X", target));
                entries.add(e);
            }
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("name", lib.isEmpty() ? "(module)" : lib);
            entry.put("version", String.format("0x%04X", version));
            entry.put("attr", String.format("0x%04X", attr));
            entry.put("functions", funcCount);
            entry.put("variables", varCount);
            entry.put("entries", entries);
            libraries.add(entry);
            a = a.add(entLen * 4L);
        }
        return libraries;
    }
}
