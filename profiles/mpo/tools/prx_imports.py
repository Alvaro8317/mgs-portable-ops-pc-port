#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lists the imports and exports of decrypted PSP modules, without Ghidra.

Reads the import (stub) and export (ent) tables that sceModuleInfo points to,
and names the NIDs with the table from build_nid_table.py when one is given.
Only library names, NIDs and function names come out, so the output can be
quoted in docs/. The input is untrusted: every offset is checked.

Usage:
  python -I profiles/mpo/tools/prx_imports.py \
      [--nids analysis/nids/ppsspp-v1.20.4.tsv] [--summary] <module.prx>...
"""

import argparse
import collections
import struct
import sys

_ELF_MAGIC = b'\x7fELF'
_PT_LOAD = 1
_MODULE_INFO_FORMAT = '<HBB28s5I'


class PrxError(Exception):
  """Raised when the module tables are malformed."""


class Module:
  """A decrypted PSP module mapped by its LOAD segments."""

  def __init__(self, data):
    if len(data) < 52 or data[:4] != _ELF_MAGIC or data[4:6] != b'\x01\x01':
      raise PrxError('not a 32-bit little-endian ELF')
    self.data = data
    e_type, = self._unpack('<H', 16)
    phoff, = self._unpack('<I', 28)
    phentsize, phnum = self._unpack('<HH', 42)
    if phentsize < 32 or phnum == 0:
      raise PrxError('invalid program header table')
    self.segments = []
    for index in range(phnum):
      fields = self._unpack('<8I', phoff + index * phentsize)
      if fields[0] == _PT_LOAD:
        self.segments.append(fields)
    if not self.segments:
      raise PrxError('no LOAD segment')
    first = self.segments[0]
    if e_type == 0xFFA0:
      info_offset = first[3] & 0x7FFFFFFF
    else:
      info_offset = first[3] - first[2] + first[1]
    (_, _, _, name, _, self.ent_top, self.ent_end, self.stub_top,
     self.stub_end) = self._unpack(_MODULE_INFO_FORMAT, info_offset)
    self.name = name.split(b'\0', 1)[0].decode('ascii', errors='replace')

  def _unpack(self, fmt, offset):
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(self.data):
      raise PrxError(f'read of {size} bytes at 0x{offset:X} is out of bounds')
    return struct.unpack_from(fmt, self.data, offset)

  def offset(self, vaddr, size=1):
    """Maps a module vaddr to a file offset (initialized bytes only)."""
    for _, p_offset, p_vaddr, _, p_filesz, _, _, _ in self.segments:
      if p_vaddr <= vaddr and vaddr + size <= p_vaddr + p_filesz:
        return p_offset + vaddr - p_vaddr
    raise PrxError(f'vaddr 0x{vaddr:X} is outside the initialized segments')

  def read(self, fmt, vaddr):
    return self._unpack(fmt, self.offset(vaddr, struct.calcsize(fmt)))

  def c_string(self, vaddr, limit=256):
    if vaddr == 0:
      return ''
    start = self.offset(vaddr)
    end = self.data.find(b'\0', start, start + limit)
    if end < 0:
      raise PrxError(f'unterminated string at 0x{vaddr:X}')
    return self.data[start:end].decode('ascii', errors='replace')

  def imports(self):
    """Yields (library, version, attr, [function NIDs], [variable NIDs])."""
    vaddr = self.stub_top
    while vaddr < self.stub_end:
      lib_ptr, version, attr, ent_len, var_count, func_count, nid_ptr = (
          self.read('<IHHBBHI', vaddr))
      if ent_len < 5:
        raise PrxError(f'bad stub entry length {ent_len} at 0x{vaddr:X}')
      funcs = list(self.read(f'<{func_count}I', nid_ptr)) if func_count else []
      variables = []
      if var_count and ent_len >= 6:
        var_ptr, = self.read('<I', vaddr + 20)
        # Each variable import is (address, NID).
        pairs = self.read(f'<{2 * var_count}I', var_ptr)
        variables = list(pairs[1::2])
      yield self.c_string(lib_ptr), version, attr, funcs, variables
      vaddr += ent_len * 4

  def exports(self):
    """Yields (library, version, attr, [function NIDs], [variable NIDs])."""
    vaddr = self.ent_top
    while vaddr < self.ent_end:
      lib_ptr, version, attr, ent_len, var_count, func_count, table = (
          self.read('<IHHBBHI', vaddr))
      if ent_len < 4:
        raise PrxError(f'bad export entry length {ent_len} at 0x{vaddr:X}')
      count = var_count + func_count
      nids = list(self.read(f'<{count}I', table)) if count else []
      yield (self.c_string(lib_ptr) or '(module)', version, attr,
             nids[:func_count], nids[func_count:])
      vaddr += ent_len * 4


def load_nids(path):
  """Returns ({(library, nid): (name, implemented)}, {nid: name})."""
  by_library = {}
  by_nid = {}
  with open(path, encoding='utf-8') as tsv:
    for line in tsv:
      fields = line.rstrip('\n').split('\t')
      if len(fields) < 4:
        continue
      nid = int(fields[0], 16)
      by_library[(fields[1], nid)] = (fields[2], fields[3] == '1')
      by_nid.setdefault(nid, fields[2])
  return by_library, by_nid


def describe(path, nids, summary):
  with open(path, 'rb') as module_file:
    module = Module(module_file.read())
  by_library, by_nid = nids
  lines = [f'{path}: module {module.name}']
  for lib, version, attr, funcs, variables in module.imports():
    status = collections.Counter()
    names = []
    for nid in funcs:
      entry = by_library.get((lib, nid))
      if entry is None:
        status['unknown'] += 1
        names.append(f'{lib}_{nid:08X}')
      else:
        status['implemented' if entry[1] else 'unimplemented'] += 1
        names.append(entry[0])
    lines.append(
        f'  import {lib} v0x{version:04X} attr 0x{attr:04X}: '
        f'{len(funcs)} functions, {len(variables)} variables '
        f'(PPSSPP: {status["implemented"]} implemented, '
        f'{status["unimplemented"]} unimplemented, {status["unknown"]} '
        f'unknown)')
    if not summary:
      lines += [f'    {name}' for name in names]
  for lib, version, attr, funcs, variables in module.exports():
    lines.append(f'  export {lib} v0x{version:04X} attr 0x{attr:04X}: '
                 f'{len(funcs)} functions, {len(variables)} variables')
    if not summary:
      lines += [f'    0x{nid:08X} {by_nid.get(nid, "?")}'
                for nid in funcs + variables]
  return lines


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('modules', nargs='+', help='decrypted ELF/PRX files')
  parser.add_argument('--nids', help='TSV from build_nid_table.py')
  parser.add_argument('--summary', action='store_true',
                      help='one line per library, without function names')
  args = parser.parse_args(argv)
  nids = load_nids(args.nids) if args.nids else ({}, {})
  status = 0
  for path in args.modules:
    try:
      print('\n'.join(describe(path, nids, args.summary)))
    except (OSError, PrxError, struct.error) as error:
      print(f'error: {path}: {error}', file=sys.stderr)
      status = 1
  return status


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
