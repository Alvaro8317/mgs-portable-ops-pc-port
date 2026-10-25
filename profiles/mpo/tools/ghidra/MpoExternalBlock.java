// SPDX-License-Identifier: GPL-3.0-or-later
// Adds a stand-in memory block for another module's address range, so calls
// from the current program into it (a stage overlay calling mgp_main after
// its .rlc is applied) disassemble cleanly. Without it, Ghidra stops at each
// such call ("flow into non-existing memory", or "not permitted within
// uninitialized memory" for an empty block), and MpoSeedFunctions rejects the
// seeds, which left a third of a mission stage's .text undecoded (F04).
//
// The block is filled with `jr ra; nop` pairs, so any call target returns at
// once (from an odd word it runs `nop; jr ra; nop`). It holds no bytes of the
// other module, and it is not executable, so the metrics, seeding and export
// scripts (which only look at executable blocks) ignore it. Decompiled code
// that reads data from that range shows the filler, not the real values.
//
// Usage (headless): -preScript MpoExternalBlock.java <name> <hex start> <hex size>
//@category MPO

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;

public class MpoExternalBlock extends GhidraScript {
    private static final int JR_RA = 0x03E00008;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) {
            throw new IllegalArgumentException("usage: <name> <hex start> <hex size>");
        }
        Address start = toAddr(Long.parseUnsignedLong(args[1], 16));
        long size = Long.parseUnsignedLong(args[2], 16);
        if (size == 0 || size > 0x02000000L || (size & 7) != 0) {
            throw new IllegalArgumentException("block size out of range: " + args[2]);
        }
        if (currentProgram.getMemory().intersects(start, start.add(size - 1))) {
            throw new IllegalArgumentException("block overlaps the program: " + args[1]);
        }
        MemoryBlock block = currentProgram.getMemory().createInitializedBlock(
            args[0], start, size, (byte) 0, monitor, false);
        byte[] filler = new byte[(int) size];
        for (int i = 0; i < filler.length; i += 8) {
            filler[i] = (byte) JR_RA;
            filler[i + 1] = (byte) (JR_RA >>> 8);
            filler[i + 2] = (byte) (JR_RA >>> 16);
            filler[i + 3] = (byte) (JR_RA >>> 24);
        }
        currentProgram.getMemory().setBytes(start, filler);
        block.setRead(true);
        block.setWrite(true);
        block.setExecute(false);
        println("EXTERNAL-BLOCK " + args[0] + " " + start + " +0x" + Long.toHexString(size));
    }
}
