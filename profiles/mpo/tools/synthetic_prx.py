# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds small synthetic PSP modules for the tool tests (no game data).

The layout follows what the real modules use: two LOAD segments (code at
vaddr 0, data after it), a PT_PRXRELOC segment, sceModuleInfo in the first
segment (its file offset in p_paddr), a syslib export, optional imports and a
section table with `.text` and `.shstrtab`.
"""

import struct

PT_LOAD = 1
PT_PRXRELOC = 0x700000A0
SYSLIB_NIDS = (0xD632ACDB, 0xCEE8593C, 0xF01D73A7)  # start, stop, info.

_HEADER_SIZE = 52
_PHDR_SIZE = 32
_SEG0_OFFSET = 0xA0


def _align(value, alignment):
  return (value + alignment - 1) & ~(alignment - 1)


def build(text_words, data=b'', bss=0, relocations=(), imports=(),
          name='synthetic', gp=0x8000):
  """Returns the bytes of a relocatable PRX.

  Args:
    text_words: list of u32 placed at vaddr 0 (the `.text` section).
    data: bytes of the second segment.
    bss: extra zeroed bytes after `data` (memsz - filesz).
    relocations: (r_offset, r_info) pairs for PT_PRXRELOC.
    imports: (library name, [function NIDs]) pairs.
    name: sceModuleInfo name.
    gp: sceModuleInfo gp value.
  """
  text = struct.pack(f'<{len(text_words)}I', *text_words)
  seg0 = bytearray(text)

  def put(blob, alignment=4):
    while len(seg0) % alignment:
      seg0.append(0)
    offset = len(seg0)
    seg0.extend(blob)
    return offset

  # Export table: syslib NIDs followed by their addresses.
  syslib_table = put(struct.pack('<6I', *SYSLIB_NIDS, 0, 0, 0))
  ent_top = put(struct.pack('<IHHBBHI', 0, 0, 0x8000, 4, 1, 2, syslib_table))
  ent_end = len(seg0)

  stub_entries = []
  for library, nids in imports:
    name_ptr = put(library.encode('ascii') + b'\0')
    nid_table = put(struct.pack(f'<{len(nids)}I', *nids))
    stub_table = put(b'\0' * (8 * len(nids)))
    stub_entries.append(
        struct.pack('<IHHBBHII', name_ptr, 0x11, 0x4001, 5, 0, len(nids),
                    nid_table, stub_table))
  stub_top = put(b''.join(stub_entries))
  stub_end = len(seg0)

  info_offset = put(
      struct.pack('<HBB28s5I', 0, 1, 1, name.encode('ascii'), gp, ent_top,
                  ent_end, stub_top, stub_end))

  seg1_vaddr = _align(len(seg0), 16)
  seg1_offset = _SEG0_OFFSET + seg1_vaddr
  reloc_offset = _align(seg1_offset + len(data), 16)
  reloc_blob = b''.join(struct.pack('<II', *r) for r in relocations)
  shstrtab = b'\0.text\0.shstrtab\0'
  shstrtab_offset = reloc_offset + len(reloc_blob)
  shoff = _align(shstrtab_offset + len(shstrtab), 4)

  header = bytearray(b'\x7fELF\x01\x01\x01' + b'\0' * 9)
  header += struct.pack('<HHIIIIIHHHHHH', 0xFFA0, 8, 1, 0, _HEADER_SIZE,
                        shoff, 0x10A23001, _HEADER_SIZE, _PHDR_SIZE, 3, 40, 3,
                        2)
  phdrs = struct.pack('<8I', PT_LOAD, _SEG0_OFFSET, 0,
                      _SEG0_OFFSET + info_offset, len(seg0), len(seg0), 5,
                      16)
  phdrs += struct.pack('<8I', PT_LOAD, seg1_offset, seg1_vaddr, 0, len(data),
                       len(data) + bss, 6, 16)
  phdrs += struct.pack('<8I', PT_PRXRELOC, reloc_offset, 0, 0, len(reloc_blob),
                       0, 0, 16)
  sections = struct.pack('<10I', *([0] * 10))
  sections += struct.pack('<10I', 1, 1, 6, 0, _SEG0_OFFSET, len(text), 0, 0,
                          16, 0)
  sections += struct.pack('<10I', 7, 3, 0, 0, shstrtab_offset, len(shstrtab),
                          0, 0, 1, 0)

  image = bytearray(shoff + len(sections))
  image[0:len(header)] = header
  image[_HEADER_SIZE:_HEADER_SIZE + len(phdrs)] = phdrs
  image[_SEG0_OFFSET:_SEG0_OFFSET + len(seg0)] = seg0
  image[seg1_offset:seg1_offset + len(data)] = data
  image[reloc_offset:reloc_offset + len(reloc_blob)] = reloc_blob
  image[shstrtab_offset:shstrtab_offset + len(shstrtab)] = shstrtab
  image[shoff:] = sections
  return bytes(image)


def info(kind, ofs_base, addr_base):
  """Returns r_info for a relocation of `kind` (R_MIPS_* number)."""
  return kind | ofs_base << 8 | addr_base << 16
