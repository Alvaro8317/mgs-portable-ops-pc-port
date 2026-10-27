#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Names the files behind the sector numbers (LBN) of a UMD image.

kjfs opens disc files as `disc0:/sce_lbn0x<lbn>_size0x<size>`, so a trace only
shows sector numbers. This walks the ISO9660 directory tree and prints the
file that starts at (or contains) each LBN given. Only paths and numbers come
out. The input is untrusted: every record is checked before it is used.

Usage:
  python -I profiles/mpo/tools/iso_lbn.py game/ULUS-10202.iso 0x39650 0xb680
  python -I profiles/mpo/tools/iso_lbn.py game/ULUS-10202.iso --trace t.txt
"""

import argparse
import re
import struct
import sys

_SECTOR = 2048
_PVD_SECTOR = 16
_MAX_DEPTH = 32
_MAX_ENTRIES = 1 << 20
_TRACE_LBN = re.compile(r'sce_lbn0x([0-9a-fA-F]+)_size0x([0-9a-fA-F]+)')


class IsoError(Exception):
  """Raised when the image is not a well-formed ISO9660 volume."""


def _read_sector(iso, lbn, count=1):
  iso.seek(lbn * _SECTOR)
  data = iso.read(count * _SECTOR)
  if len(data) != count * _SECTOR:
    raise IsoError(f'sector {lbn:#x} is past the end of the image')
  return data


def _records(data):
  """Yields (lbn, size, is_dir, name) for each directory record in `data`."""
  offset = 0
  while offset < len(data):
    length = data[offset]
    if length == 0:
      # Records do not cross sectors: skip to the next one.
      offset = (offset // _SECTOR + 1) * _SECTOR
      continue
    if length < 34 or offset + length > len(data):
      raise IsoError(f'bad directory record at {offset:#x}')
    lbn, = struct.unpack_from('<I', data, offset + 2)
    size, = struct.unpack_from('<I', data, offset + 10)
    flags = data[offset + 25]
    name_length = data[offset + 32]
    if 33 + name_length > length:
      raise IsoError(f'bad name length at {offset:#x}')
    name = data[offset + 33:offset + 33 + name_length]
    yield lbn, size, bool(flags & 2), name
    offset += length


def list_files(iso):
  """Returns [(lbn, size, path)] for every file of the volume."""
  pvd = _read_sector(iso, _PVD_SECTOR)
  if pvd[1:6] != b'CD001' or pvd[0] != 1:
    raise IsoError('no primary volume descriptor')
  root_lbn, = struct.unpack_from('<I', pvd, 156 + 2)
  root_size, = struct.unpack_from('<I', pvd, 156 + 10)
  files = []
  pending = [(root_lbn, root_size, '', 0)]
  seen = set()
  while pending:
    lbn, size, path, depth = pending.pop()
    if depth > _MAX_DEPTH or lbn in seen:
      continue
    seen.add(lbn)
    data = _read_sector(iso, lbn, (size + _SECTOR - 1) // _SECTOR)
    for child_lbn, child_size, is_dir, name in _records(data):
      if name in (b'\x00', b'\x01'):
        continue
      text = name.decode('ascii', 'replace').split(';')[0]
      child = f'{path}/{text}' if path else text
      if is_dir:
        pending.append((child_lbn, child_size, child, depth + 1))
      else:
        files.append((child_lbn, child_size, child))
      if len(files) > _MAX_ENTRIES:
        raise IsoError('too many files')
  return sorted(files)


def lookup(files, lbn):
  """Returns (path, offset in bytes) of the file holding sector `lbn`."""
  for start, size, path in files:
    if start <= lbn < start + max(1, (size + _SECTOR - 1) // _SECTOR):
      return path, (lbn - start) * _SECTOR
  return None, 0


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('iso')
  parser.add_argument('lbns', nargs='*', help='sector numbers (0x... or decimal)')
  parser.add_argument('--trace', help='names every sce_lbn of an mpo_boot trace')
  args = parser.parse_args(argv)
  wanted = [int(text, 0) for text in args.lbns]
  if args.trace:
    with open(args.trace, encoding='utf-8', errors='replace') as trace:
      for line in trace:
        for match in _TRACE_LBN.finditer(line):
          lbn = int(match.group(1), 16)
          if lbn not in wanted:
            wanted.append(lbn)
  with open(args.iso, 'rb') as iso:
    files = list_files(iso)
  for lbn in wanted:
    path, offset = lookup(files, lbn)
    where = f'{path} +{offset:#x}' if offset else path
    print(f'{lbn:#x}\t{where or "(no file)"}')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
