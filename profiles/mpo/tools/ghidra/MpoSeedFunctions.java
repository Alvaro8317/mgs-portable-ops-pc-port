// SPDX-License-Identifier: GPL-3.0-or-later
// Seeds functions at code addresses that only data points to, so that the
// following auto-analysis disassembles them. Ghidra only follows control flow
// from the entry points, so code reached exclusively through function pointers
// (vtables, callback tables, thread entries) can stay undefined.
//
// Candidates come from the PSP relocations, which are exact:
//   - a relocated 32-bit word in a non-executable block whose value points
//     into an executable block ("data" candidates: tables of pointers);
//   - a non-flow reference from an instruction into an executable block
//     ("code" candidates: lui/addiu pairs that build a code address, for
//     example the entry of sceKernelCreateThread).
//
// With the "prologues" mode it also seeds, inside the bytes that are still
// undefined in an executable block, every `addiu sp, sp, -N` (a stack-frame
// prologue). That finds functions nothing points to: dead code or code only
// reached from outside the module (for example, from a stage overlay).
//
// With the "gaps" mode it seeds the first non-zero word of every run of bytes
// that is still undefined in an executable block (leaf functions without a
// stack frame), and the first instruction of every run of disassembled code
// that no function owns (targets of `j` tail calls). GCC keeps no data in
// .text, so these runs should be code; a seed whose disassembly raises an
// error bookmark is undone and counted as "rejected".
//
// A candidate is seeded only if it is 4-aligned, it is not inside an existing
// function body (so jump-table case labels are skipped), and the previous word
// ends a function or is padding, i.e. the address looks like a function start.
// Everything else is counted and left alone. The counts go to
// <outDir>/<program>.seed-<mode>.json.
//
// Runs after the first auto-analysis (so the code references exist), re-runs
// the analysis on the changes and repeats until a round seeds nothing (at most
// 16 rounds).
//
// Usage (headless): -postScript MpoSeedFunctions.java <outDir> relocations|prologues|gaps
//@category MPO

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkManager;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.reloc.Relocation;
import ghidra.program.model.symbol.Reference;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;

public class MpoSeedFunctions extends GhidraScript {
    private static final int JR_RA = 0x03E00008;
    private static final int ADDIU_SP_NEGATIVE = 0x27BD8000; // addiu sp, sp, imm < 0
    private static final int MAX_ROUNDS = 16;
    private int lastRelocationCount;
    private final TreeSet<Long> attempted = new TreeSet<>();

    private boolean inExec(Address a) {
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        return b != null && b.isExecute() && b.isInitialized();
    }

    @Override
    public void run() throws Exception {
        Path outDir = Path.of(getScriptArgs()[0]);
        String mode = getScriptArgs().length > 1 ? getScriptArgs()[1] : "relocations";
        if (!mode.equals("relocations") && !mode.equals("prologues") && !mode.equals("gaps")) {
            throw new IllegalArgumentException("mode must be relocations, prologues or gaps: " + mode);
        }
        Files.createDirectories(outDir);
        java.util.List<Object> rounds = new java.util.ArrayList<>();
        int total = 0;
        for (int round = 1; round <= MAX_ROUNDS; round++) {
            TreeMap<Long, String> candidates = mode.equals("relocations") ? relocationCandidates()
                : undefinedCandidates(mode.equals("gaps"));
            Map<String, Object> result = seed(candidates);
            rounds.add(result);
            int seeded = (Integer) result.get("seeded");
            total += seeded;
            println("SEED " + currentProgram.getName() + " round " + round + ": " + result);
            if (seeded == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("program", currentProgram.getName());
        out.put("mode", mode);
        out.put("seeded_total", total);
        out.put("relocations", lastRelocationCount);
        out.put("functions_after", currentProgram.getFunctionManager().getFunctionCount());
        out.put("rounds", rounds);
        Files.writeString(outDir.resolve(currentProgram.getName() + ".seed-" + mode + ".json"), MpoJson.value(out),
            StandardCharsets.UTF_8);
    }

    /**
     * Undefined words in executable blocks (except import stubs): every
     * `addiu sp, sp, -N`, or (gaps) the first non-zero word of each run.
     */
    private TreeMap<Long, String> undefinedCandidates(boolean gaps) throws Exception {
        Memory mem = currentProgram.getMemory();
        TreeMap<Long, String> candidates = new TreeMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isExecute() || !b.isInitialized() || b.getName().equals(".sceStub.text")) {
                continue;
            }
            AddressSet range = new AddressSet(b.getStart(), b.getEnd());
            AddressSet code = new AddressSet();
            for (Instruction ins : currentProgram.getListing().getInstructions(range, true)) {
                code.add(ins.getMinAddress(), ins.getMaxAddress());
            }
            for (AddressRange r : range.subtract(code)) {
                Address a = r.getMinAddress();
                long misalign = a.getOffset() & 3;
                if (misalign != 0) {
                    a = a.add(4 - misalign);
                }
                for (; a.add(3).compareTo(r.getMaxAddress()) <= 0; a = a.add(4)) {
                    monitor.checkCancelled();
                    int word = mem.getInt(a);
                    if (gaps && word != 0) {
                        candidates.put(a.getOffset(), "gap");
                        break;
                    }
                    if (!gaps && (word & 0xFFFF8000) == ADDIU_SP_NEGATIVE) {
                        candidates.put(a.getOffset(), "prologue");
                    }
                }
            }
            if (gaps) {
                // Disassembled code that no function owns (typically the target of a
                // `j` tail call): its first instruction becomes a function entry.
                AddressSet owned = new AddressSet();
                for (Function f : currentProgram.getFunctionManager().getFunctions(range, true)) {
                    owned.add(f.getBody());
                }
                for (AddressRange r : code.subtract(owned)) {
                    candidates.putIfAbsent(r.getMinAddress().getOffset(), "orphan");
                }
            }
        }
        return candidates;
    }

    private TreeMap<Long, String> relocationCandidates() throws Exception {
        Memory mem = currentProgram.getMemory();

        // address -> origin ("data" or "code")
        TreeMap<Long, String> candidates = new TreeMap<>();
        int relocations = 0;
        Iterator<Relocation> it = currentProgram.getRelocationTable().getRelocations();
        while (it.hasNext()) {
            monitor.checkCancelled();
            Relocation r = it.next();
            relocations++;
            Address at = r.getAddress();
            MemoryBlock block = mem.getBlock(at);
            if (block == null || !block.isInitialized()) {
                continue;
            }
            if (!block.isExecute()) {
                if ((at.getOffset() & 3) != 0) {
                    continue;
                }
                long value = mem.getInt(at) & 0xFFFFFFFFL;
                Address target;
                try {
                    target = toAddr(value);
                } catch (Exception e) {
                    continue;
                }
                if (inExec(target)) {
                    candidates.putIfAbsent(value, "data");
                }
                continue;
            }
            for (Reference ref : getReferencesFrom(at)) {
                if (ref.getReferenceType().isFlow() || !ref.getToAddress().isMemoryAddress()) {
                    continue;
                }
                if (inExec(ref.getToAddress())) {
                    candidates.putIfAbsent(ref.getToAddress().getOffset(), "code");
                }
            }
        }
        lastRelocationCount = relocations;
        return candidates;
    }

    private Map<String, Object> seed(TreeMap<Long, String> candidates) throws Exception {
        Map<String, Integer> counts = new TreeMap<>();
        TreeSet<Long> seededAt = new TreeSet<>();
        for (Map.Entry<Long, String> c : candidates.entrySet()) {
            monitor.checkCancelled();
            Address a = toAddr(c.getKey());
            String outcome;
            if ((c.getKey() & 3) != 0) {
                outcome = "unaligned";
            } else if (attempted.contains(c.getKey())) {
                // Seeded or rejected in an earlier round; if the analysis undid the
                // function (for example by merging it), do not loop on it.
                outcome = "already_tried";
            } else if (getFunctionAt(a) != null) {
                outcome = "already_function";
            } else if (getFunctionContaining(a) != null) {
                outcome = "inside_function";
            } else if (!looksLikeStart(a)) {
                outcome = "not_a_start";
            } else if (getInstructionAt(a) == null && !disassembleCleanly(a)) {
                outcome = "rejected";
                attempted.add(c.getKey());
            } else {
                attempted.add(c.getKey());
                Function f = getInstructionAt(a) == null ? null : createFunction(a, null);
                outcome = f != null ? "seeded" : "failed";
                if (f != null) {
                    seededAt.add(c.getKey());
                }
            }
            counts.merge(c.getValue() + "." + outcome, 1, Integer::sum);
        }

        Map<String, Object> out = new LinkedHashMap<>();
        out.put("candidates", candidates.size());
        out.put("outcomes", counts);
        out.put("seeded", seededAt.size());
        return out;
    }

    /** Disassembles from `a`; undoes it and returns false if that raised errors. */
    private boolean disassembleCleanly(Address a) throws Exception {
        DisassembleCommand cmd = new DisassembleCommand(a, null, true);
        cmd.applyTo(currentProgram, monitor);
        AddressSet done = cmd.getDisassembledAddressSet();
        if (done == null || done.isEmpty()) {
            return false;
        }
        BookmarkManager bookmarks = currentProgram.getBookmarkManager();
        AddressSet errors = new AddressSet();
        for (AddressRange r : done) {
            Address end = r.getMaxAddress().add(4);
            java.util.Iterator<Bookmark> it = bookmarks.getBookmarksIterator(r.getMinAddress(), true);
            while (it.hasNext()) {
                Bookmark b = it.next();
                if (b.getAddress().compareTo(end) > 0) {
                    break;
                }
                if (b.getTypeString().equals(BookmarkType.ERROR)) {
                    errors.add(b.getAddress());
                    bookmarks.removeBookmark(b);
                }
            }
        }
        if (errors.isEmpty()) {
            return true;
        }
        clearListing(done);
        return false;
    }

    /** True if the word before `a` ends a function (jr ra + delay slot) or is padding. */
    private boolean looksLikeStart(Address a) throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock block = mem.getBlock(a);
        if (a.equals(block.getStart())) {
            return true;
        }
        Address prev = a.subtract(4);
        Function before = getFunctionContaining(prev);
        if (before != null) {
            return true; // Previous bytes belong to another function: `a` starts a new one.
        }
        int prevWord = mem.getInt(prev);
        if (prevWord == 0) {
            return true; // Padding (nop) or the delay slot of a preceding return.
        }
        if (a.subtract(8).compareTo(block.getStart()) >= 0 && mem.getInt(a.subtract(8)) == JR_RA) {
            return true;
        }
        Instruction ins = getInstructionAt(prev);
        return ins != null && !ins.getFlowType().isFallthrough() && !ins.isInDelaySlot();
    }
}
