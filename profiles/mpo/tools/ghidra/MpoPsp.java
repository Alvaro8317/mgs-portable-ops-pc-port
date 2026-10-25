// SPDX-License-Identifier: GPL-3.0-or-later
// PSP module helpers shared by the MPO Ghidra scripts.
//@category MPO

import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;

import java.util.TreeMap;

public final class MpoPsp {
    private MpoPsp() {}

    /**
     * Returns import stub address -> "library:NID" for every function import in
     * the .lib.stub table. The stubs are not always in .sceStub.text: mgp_main
     * keeps the stubs of the Konami modules (kjfs, sound2, zlibdec) in .text.
     */
    public static TreeMap<Long, String> importStubs(Program program) throws MemoryAccessException {
        TreeMap<Long, String> stubs = new TreeMap<>();
        Memory mem = program.getMemory();
        MemoryBlock table = mem.getBlock(".lib.stub");
        if (table == null) {
            return stubs;
        }
        AddressSpace space = program.getAddressFactory().getDefaultAddressSpace();
        Address a = table.getStart();
        while (a.compareTo(table.getEnd()) < 0) {
            int entLen = mem.getByte(a.add(8)) & 0xFF;
            if (entLen < 5) {
                throw new IllegalStateException("bad stub entry length " + entLen + " at " + a);
            }
            String lib = cString(mem, space.getAddress(mem.getInt(a) & 0xFFFFFFFFL));
            int funcCount = mem.getShort(a.add(10)) & 0xFFFF;
            long nidPtr = mem.getInt(a.add(12)) & 0xFFFFFFFFL;
            long stubPtr = mem.getInt(a.add(16)) & 0xFFFFFFFFL;
            for (int i = 0; i < funcCount; i++) {
                long nid = mem.getInt(space.getAddress(nidPtr + 4L * i)) & 0xFFFFFFFFL;
                stubs.put(stubPtr + 8L * i, String.format("%s:%08X", lib, nid));
            }
            a = a.add(entLen * 4L);
        }
        return stubs;
    }

    /** Reads a NUL-terminated ASCII string of at most 256 bytes. */
    public static String cString(Memory mem, Address a) throws MemoryAccessException {
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
}
