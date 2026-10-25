#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prints the identity of a decrypted PSP module: ELF header, program headers
and sceModuleInfo.

Only numbers and names come out (no code or data from the module), so the
output can be quoted in docs/. The input is untrusted: every offset and size is
checked before it is used.

Usage:
  python -I profiles/mpo/tools/prx_info.py game/elf/mgp_main.elf
"""

import argparse
import struct
import sys

_ELF_MAGIC = b'\x7fELF'
_ELF_TYPES = {
    0x0002: 'ET_EXEC (fixed address)',
    0xFFA0: 'PRX (relocatable)',
}
_MACHINE_MIPS = 8
_PROGRAM_TYPES = {
    0x00000001: 'LOAD',
    0x700000A0: 'PRX_RELOC',
    0x700000A1: 'PRX_RELOC2',
}
_MODULE_INFO_FORMAT = '<HBB28s5I'  # attr, minor, major, name, gp, ent/stub.
_MODULE_INFO_SIZE = struct.calcsize(_MODULE_INFO_FORMAT)


class ElfError(Exception):
  """Raised when the module is not a well-formed little-endian MIPS ELF."""


def _unpack(fmt, data, offset):
  """Unpacks `fmt` at `offset`, checking that it fits inside `data`."""
  size = struct.calcsize(fmt)
  if offset < 0 or offset + size > len(data):
    raise ElfError(f'read of {size} bytes at 0x{offset:X} is out of bounds')
  return struct.unpack_from(fmt, data, offset)


def _c_string(raw):
  return raw.split(b'\0', 1)[0].decode('ascii', errors='replace')


def _section_names(data, header):
  """Returns the section names, or [] if the module has no section table."""
  shoff, shentsize, shnum, shstrndx = (header['shoff'], header['shentsize'],
                                       header['shnum'], header['shstrndx'])
  if shoff == 0 or shnum == 0:
    return []
  if shentsize < 40 or shstrndx >= shnum:
    raise ElfError('invalid section header table')
  sections = [
      _unpack('<10I', data, shoff + index * shentsize) for index in range(shnum)
  ]
  strtab_offset, strtab_size = sections[shstrndx][4], sections[shstrndx][5]
  if strtab_offset + strtab_size > len(data):
    raise ElfError('section name table is out of bounds')
  strtab = data[strtab_offset:strtab_offset + strtab_size]
  names = []
  for section in sections:
    name_offset = section[0]
    if name_offset >= len(strtab):
      raise ElfError('section name is out of bounds')
    names.append(_c_string(strtab[name_offset:]))
  return names


def describe(data):
  """Returns a list of report lines for the module in `data`."""
  if len(data) < 52 or data[:4] != _ELF_MAGIC:
    raise ElfError('not an ELF file')
  if data[4] != 1 or data[5] != 1:
    raise ElfError('not a 32-bit little-endian ELF')
  fields = _unpack('<HHIIIIIHHHHHH', data, 16)
  header = dict(
      zip(('type', 'machine', 'version', 'entry', 'phoff', 'shoff', 'flags',
           'ehsize', 'phentsize', 'phnum', 'shentsize', 'shnum', 'shstrndx'),
          fields))
  if header['machine'] != _MACHINE_MIPS:
    raise ElfError(f'machine {header["machine"]} is not MIPS')
  if header['phentsize'] < 32 or header['phnum'] == 0:
    raise ElfError('invalid program header table')

  lines = [
      f'file size      {len(data)} bytes',
      f'e_type         0x{header["type"]:04X} '
      f'{_ELF_TYPES.get(header["type"], "(unknown)")}',
      f'e_flags        0x{header["flags"]:08X}',
      f'e_entry        0x{header["entry"]:08X}',
      'program headers:',
  ]
  segments = []
  for index in range(header['phnum']):
    segment = _unpack('<8I', data, header['phoff'] + index * header['phentsize'])
    segments.append(segment)
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = (
        segment)
    lines.append(
        f'  [{index}] {_PROGRAM_TYPES.get(p_type, f"0x{p_type:08X}"):10} '
        f'offset=0x{p_offset:06X} vaddr=0x{p_vaddr:08X} paddr=0x{p_paddr:08X} '
        f'filesz=0x{p_filesz:06X} memsz=0x{p_memsz:06X} flags={p_flags} '
        f'align=0x{p_align:X}')

  # In a PRX the first segment's p_paddr is the file offset of sceModuleInfo;
  # in a fixed-address ELF it is its physical address.
  first = segments[0]
  if header['type'] == 0xFFA0:
    info_offset = first[3] & 0x7FFFFFFF
  else:
    info_offset = first[3] - first[2] + first[1]
  attr, minor, major, name, gp, ent_top, ent_end, stub_top, stub_end = _unpack(
      _MODULE_INFO_FORMAT, data, info_offset)
  info_vaddr = info_offset - first[1] + first[2]
  lines += [
      'sceModuleInfo:',
      f'  file offset  0x{info_offset:06X} (vaddr 0x{info_vaddr:08X})',
      f'  name         {_c_string(name)}',
      f'  attribute    0x{attr:04X}',
      f'  version      {major}.{minor}',
      f'  gp           0x{gp:08X}',
      f'  exports      0x{ent_top:08X}-0x{ent_end:08X} '
      f'({(ent_end - ent_top) & 0xFFFFFFFF} bytes)',
      f'  imports      0x{stub_top:08X}-0x{stub_end:08X} '
      f'({(stub_end - stub_top) & 0xFFFFFFFF} bytes)',
  ]

  names = _section_names(data, header)
  lines.append(f'sections       {len(names)}')
  if names:
    lines.append('  ' + ' '.join(name for name in names if name))
  return lines


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('module', help='decrypted ELF/PRX file')
  args = parser.parse_args(argv)
  with open(args.module, 'rb') as module_file:
    data = module_file.read()
  try:
    lines = describe(data)
  except ElfError as error:
    print(f'error: {args.module}: {error}', file=sys.stderr)
    return 1
  print('\n'.join(lines))
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
