// SPDX-License-Identifier: GPL-3.0-or-later
// Locates the analysis anchors listed in an anchors file and writes
// <outDir>/<program>.anchors.json with, for each anchor, the matching
// addresses and the functions that reference them. It also writes
// <outDir>/<program>.strings.tsv (every defined string with its address and
// reference count) for local exploration; that file holds game text and must
// stay in the ignored analysis/ directory.
//
// Anchors file (profiles/mpo/config/anchors.txt), one per line, '#' comments:
//   string  <label> <java regex>    defined strings that match (find())
//   address <label> <hex address>   references to that address (read/write)
//   import  <label> <java regex>    call sites of import stubs whose name matches
//
// Usage (headless): -postScript MpoFindAnchors.java <anchors.txt> <outDir>
//@category MPO

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
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
import java.util.TreeMap;
import java.util.regex.Pattern;

public class MpoFindAnchors extends GhidraScript {
    private record Str(Address address, String text) {}

    private final List<Str> strings = new ArrayList<>();

    private static String hex(long v) {
        return String.format("0x%08X", v);
    }

    private String functionAt(Address from) {
        Function f = getFunctionContaining(from);
        return f == null ? "(no function) " + hex(from.getOffset())
            : hex(f.getEntryPoint().getOffset()) + " " + f.getName();
    }

    /** Groups references to `to` by referencing function, with their kinds. */
    private Map<String, Object> referencesTo(Address to) {
        Map<String, List<String>> byFunction = new TreeMap<>();
        for (Reference r : getReferencesTo(to)) {
            String kind = r.getReferenceType().isWrite() ? "write"
                : r.getReferenceType().isRead() ? "read"
                : r.getReferenceType().isCall() ? "call"
                : r.getReferenceType().isFlow() ? "flow" : "data";
            MemoryBlock b = currentProgram.getMemory().getBlock(r.getFromAddress());
            String where = b != null && !b.isExecute() ? "data " + b.getName() + " " + hex(r.getFromAddress().getOffset())
                : functionAt(r.getFromAddress());
            byFunction.computeIfAbsent(where, k -> new ArrayList<>()).add(kind);
        }
        return new LinkedHashMap<>(byFunction);
    }

    private void loadStrings(Path outDir) throws Exception {
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        try (BufferedWriter w = Files.newBufferedWriter(outDir.resolve(currentProgram.getName() + ".strings.tsv"),
                StandardCharsets.UTF_8)) {
            while (it.hasNext()) {
                monitor.checkCancelled();
                Data d = it.next();
                if (!d.hasStringValue()) {
                    continue;
                }
                String text = StringDataInstance.getStringDataInstance(d).getStringValue();
                if (text == null) {
                    continue;
                }
                strings.add(new Str(d.getAddress(), text));
                int refs = getReferencesTo(d.getAddress()).length;
                w.write(hex(d.getAddress().getOffset()) + "\t" + refs + "\t"
                    + text.replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r") + "\n");
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path outDir = Path.of(args[1]);
        Files.createDirectories(outDir);
        loadStrings(outDir);

        List<Object> anchors = new ArrayList<>();
        for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            String[] f = line.split("\\s+", 3);
            if (f.length != 3) {
                throw new IllegalArgumentException("bad anchor line: " + line);
            }
            Map<String, Object> a = new LinkedHashMap<>();
            a.put("kind", f[0]);
            a.put("label", f[1]);
            a.put("query", f[2]);
            List<Object> matches = new ArrayList<>();
            switch (f[0]) {
                case "string" -> {
                    Pattern p = Pattern.compile(f[2]);
                    for (Str s : strings) {
                        if (p.matcher(s.text()).find()) {
                            Map<String, Object> m = new LinkedHashMap<>();
                            m.put("address", hex(s.address().getOffset()));
                            m.put("text", s.text().length() > 96 ? s.text().substring(0, 96) + "..." : s.text());
                            m.put("referenced_by", referencesTo(s.address()));
                            matches.add(m);
                        }
                    }
                }
                case "address" -> {
                    Address to = toAddr(Long.parseLong(f[2].replaceFirst("^0[xX]", ""), 16));
                    Map<String, Object> m = new LinkedHashMap<>();
                    m.put("address", hex(to.getOffset()));
                    MemoryBlock b = currentProgram.getMemory().getBlock(to);
                    m.put("block", b == null ? null : b.getName());
                    m.put("referenced_by", referencesTo(to));
                    matches.add(m);
                }
                case "import" -> {
                    Pattern p = Pattern.compile(f[2]);
                    for (Map.Entry<Long, String> stub : MpoPsp.importStubs(currentProgram).entrySet()) {
                        Function fn = getFunctionAt(toAddr(stub.getKey()));
                        String name = fn == null ? stub.getValue() : fn.getName();
                        if (p.matcher(name).find()) {
                            Map<String, Object> m = new LinkedHashMap<>();
                            m.put("import", name);
                            m.put("nid", stub.getValue());
                            m.put("stub", hex(stub.getKey()));
                            m.put("referenced_by", referencesTo(toAddr(stub.getKey())));
                            matches.add(m);
                        }
                    }
                }
                default -> throw new IllegalArgumentException("unknown anchor kind: " + f[0]);
            }
            a.put("matches", matches);
            anchors.add(a);
        }
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("program", currentProgram.getName());
        out.put("strings", strings.size());
        out.put("anchors", anchors);
        Files.writeString(outDir.resolve(currentProgram.getName() + ".anchors.json"), MpoJson.value(out),
            StandardCharsets.UTF_8);
        println("ANCHORS " + currentProgram.getName() + ": " + anchors.size() + " anchors, " + strings.size()
            + " strings");
    }
}
