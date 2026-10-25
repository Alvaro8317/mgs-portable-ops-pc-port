#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structured reader for decrypted PSP modules (relocatable PRX, e_type 0xFFA0).

Returns segments, sections, sceModuleInfo, imported and exported libraries
and the classic PRX relocations (PT_PRXRELOC, 0x700000A0). It is a library for
the other tools (rlc.py, stage_inventory.py); prx_info.py stays the quick
human-readable dump.

The input is untrusted: every offset, size and count is checked before it is
used, and errors raise PrxError. Nothing from the module is returned except
numbers and names.
"""

import collections
import struct

ELF_MAGIC = b'\x7fELF'
ET_PRX = 0xFFA0
EM_MIPS = 8
PT_LOAD = 0x00000001
PT_PRXRELOC = 0x700000A0
PT_PRXRELOC2 = 0x700000A1

# A module never has more than a handful of segments or a few hundred
# libraries; these caps stop a corrupt count from causing huge loops.
_MAX_PROGRAM_HEADERS = 64
_MAX_SECTIONS = 4096
_MAX_LIBRARIES = 1024
_MAX_NAME = 256

# Well-known syslib export NIDs (pspsdk names).
SYSLIB_NIDS = {
    0xD632ACDB: 'module_start',
    0xCEE8593C: 'module_stop',
    0xF01D73A7: 'module_info',
    0x0F7C276C: 'module_start_thread_parameter',
    0xCF0CC697: 'module_stop_thread_parameter',
    0xD3744BE0: 'module_bootstart',
    0x2F064FA6: 'module_reboot_before',
    0x11B97506: 'module_sdk_version',
}

Segment = collections.namedtuple(
    'Segment', 'type offset vaddr paddr filesz memsz flags align')
Section = collections.namedtuple('Section', 'name type addr offset size')
ModuleInfo = collections.namedtuple(
    'ModuleInfo', 'file_offset attribute major minor name gp ent_top ent_end '
    'stub_top stub_end')
Library = collections.namedtuple(
    'Library', 'name version attribute function_nids variable_nids')
Relocation = collections.namedtuple('Relocation', 'offset info')


class PrxError(Exception):
  """Raised when the module is not a well-formed little-endian MIPS PRX."""


def unpack(fmt, data, offset):
  """Unpacks `fmt` at `offset`, checking that it fits inside `data`."""
  size = struct.calcsize(fmt)
  if offset < 0 or offset + size > len(data):
    raise PrxError(f'read of {size} bytes at 0x{offset:X} is out of bounds')
  return struct.unpack_from(fmt, data, offset)


def _c_string(data, offset, limit=_MAX_NAME):
  if offset < 0 or offset >= len(data):
    raise PrxError(f'string at 0x{offset:X} is out of bounds')
  end = data.find(b'\0', offset, min(len(data), offset + limit))
  if end < 0:
    raise PrxError(f'string at 0x{offset:X} is not terminated')
  return data[offset:end].decode('ascii', errors='replace')


class Prx:
  """A parsed relocatable PRX. Addresses are module-relative vaddrs."""

  def __init__(self, data):
    self.data = bytes(data)
    self.header = self._parse_header()
    self.segments = self._parse_segments()
    self.load_segments = [s for s in self.segments if s.type == PT_LOAD]
    if not self.load_segments:
      raise PrxError('module has no LOAD segment')
    self.sections = self._parse_sections()
    self.module_info = self._parse_module_info()
    self.imports = self._parse_imports()
    self.exports = self._parse_exports()
    self.relocations = self._parse_relocations()

  # -- ELF structure ---------------------------------------------------------

  def _parse_header(self):
    data = self.data
    if len(data) < 52 or data[:4] != ELF_MAGIC:
      raise PrxError('not an ELF file')
    if data[4] != 1 or data[5] != 1:
      raise PrxError('not a 32-bit little-endian ELF')
    names = ('type', 'machine', 'version', 'entry', 'phoff', 'shoff', 'flags',
             'ehsize', 'phentsize', 'phnum', 'shentsize', 'shnum', 'shstrndx')
    header = dict(zip(names, unpack('<HHIIIIIHHHHHH', data, 16)))
    if header['machine'] != EM_MIPS:
      raise PrxError(f'machine {header["machine"]} is not MIPS')
    if header['type'] != ET_PRX:
      raise PrxError(f'e_type 0x{header["type"]:04X} is not a relocatable PRX')
    if header['phentsize'] < 32 or not (0 < header['phnum'] <=
                                         _MAX_PROGRAM_HEADERS):
      raise PrxError('invalid program header table')
    return header

  def _parse_segments(self):
    header = self.header
    segments = []
    for index in range(header['phnum']):
      segment = Segment(*unpack('<8I', self.data,
                                header['phoff'] + index * header['phentsize']))
      if segment.type in (PT_LOAD, PT_PRXRELOC, PT_PRXRELOC2):
        if segment.offset + segment.filesz > len(self.data):
          raise PrxError(f'segment {index} is out of bounds')
      if segment.type == PT_LOAD and segment.filesz > segment.memsz:
        raise PrxError(f'segment {index} has filesz > memsz')
      segments.append(segment)
    return segments

  def _parse_sections(self):
    header = self.header
    shoff, shentsize, shnum, shstrndx = (header['shoff'], header['shentsize'],
                                         header['shnum'], header['shstrndx'])
    if shoff == 0 or shnum == 0:
      return []
    if shentsize < 40 or shnum > _MAX_SECTIONS or shstrndx >= shnum:
      raise PrxError('invalid section header table')
    raw = [
        unpack('<10I', self.data, shoff + index * shentsize)
        for index in range(shnum)
    ]
    strtab_offset, strtab_size = raw[shstrndx][4], raw[shstrndx][5]
    if strtab_offset + strtab_size > len(self.data):
      raise PrxError('section name table is out of bounds')
    strtab = self.data[strtab_offset:strtab_offset + strtab_size]
    sections = []
    for entry in raw:
      if entry[0] >= max(len(strtab), 1):
        raise PrxError('section name is out of bounds')
      name = _c_string(strtab, entry[0]) if strtab else ''
      sections.append(
          Section(name=name, type=entry[1], addr=entry[3], offset=entry[4],
                  size=entry[5]))
    return sections

  def section(self, name):
    """Returns the first section called `name`, or None."""
    for section in self.sections:
      if section.name == name:
        return section
    return None

  # -- Address helpers -------------------------------------------------------

  def file_offset(self, vaddr, size=1):
    """Maps a module vaddr to a file offset (only file-backed bytes)."""
    for segment in self.load_segments:
      end = segment.vaddr + segment.filesz
      if segment.vaddr <= vaddr and vaddr + size <= end:
        return segment.offset + vaddr - segment.vaddr
    raise PrxError(f'vaddr 0x{vaddr:X} is not backed by the file')

  def read_u32(self, vaddr):
    return unpack('<I', self.data, self.file_offset(vaddr, 4))[0]

  def string_at(self, vaddr):
    return _c_string(self.data, self.file_offset(vaddr))

  def image(self):
    """Returns the loaded image (segments at their vaddrs, bss zeroed)."""
    end = max(s.vaddr + s.memsz for s in self.load_segments)
    if end > 64 * 1024 * 1024:
      raise PrxError('module image is larger than PSP RAM')
    image = bytearray(end)
    for segment in self.load_segments:
      image[segment.vaddr:segment.vaddr + segment.filesz] = self.data[
          segment.offset:segment.offset + segment.filesz]
    return image

  # -- PSP structures --------------------------------------------------------

  def _parse_module_info(self):
    first = self.load_segments[0]
    # In a PRX the first segment's p_paddr is the file offset of sceModuleInfo.
    offset = first.paddr & 0x7FFFFFFF
    fields = unpack('<HBB28s5I', self.data, offset)
    attribute, minor, major, name = fields[:4]
    return ModuleInfo(file_offset=offset, attribute=attribute, major=major,
                      minor=minor, name=name.split(b'\0', 1)[0].decode(
                          'ascii', errors='replace'), gp=fields[4],
                      ent_top=fields[5], ent_end=fields[6], stub_top=fields[7],
                      stub_end=fields[8])

  def _nid_list(self, vaddr, count):
    return [self.read_u32(vaddr + 4 * index) for index in range(count)]

  def _check_table(self, top, end, what):
    if end < top or end - top > _MAX_LIBRARIES * 32:
      raise PrxError(f'invalid {what} table 0x{top:X}-0x{end:X}')

  def _parse_imports(self):
    info = self.module_info
    self._check_table(info.stub_top, info.stub_end, 'import')
    libraries = []
    cursor = info.stub_top
    while cursor < info.stub_end:
      fields = unpack('<IHHBBHII', self.data, self.file_offset(cursor, 20))
      name_ptr, version, attribute, length, var_count, func_count = fields[:6]
      nid_table = fields[6]
      if length < 5:
        raise PrxError(f'import entry at 0x{cursor:X} has length {length}')
      name = self.string_at(name_ptr) if name_ptr else ''
      nids = self._nid_list(nid_table, func_count + var_count)
      libraries.append(
          Library(name=name, version=version, attribute=attribute,
                  function_nids=nids[:func_count],
                  variable_nids=nids[func_count:]))
      cursor += 4 * length
    return libraries

  def _parse_exports(self):
    info = self.module_info
    self._check_table(info.ent_top, info.ent_end, 'export')
    libraries = []
    cursor = info.ent_top
    while cursor < info.ent_end:
      name_ptr, version, attribute, length, var_count, func_count, table = (
          unpack('<IHHBBHI', self.data, self.file_offset(cursor, 16)))
      if length < 4:
        raise PrxError(f'export entry at 0x{cursor:X} has length {length}')
      name = self.string_at(name_ptr) if name_ptr else ''
      nids = self._nid_list(table, func_count + var_count)
      libraries.append(
          Library(name=name, version=version, attribute=attribute,
                  function_nids=nids[:func_count],
                  variable_nids=nids[func_count:]))
      cursor += 4 * length
    return libraries

  def _parse_relocations(self):
    relocations = []
    for segment in self.segments:
      if segment.type == PT_PRXRELOC2:
        raise PrxError('compressed PRX relocations (0x700000A1) are not '
                       'supported')
      if segment.type != PT_PRXRELOC:
        continue
      if segment.filesz % 8:
        raise PrxError('PRX relocation segment size is not a multiple of 8')
      for offset in range(segment.offset, segment.offset + segment.filesz, 8):
        relocations.append(Relocation(*unpack('<II', self.data, offset)))
    return relocations
