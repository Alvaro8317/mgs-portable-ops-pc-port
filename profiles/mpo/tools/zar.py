#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads a stage archive (`stage/<s>/_zar`) and the DAR archives inside it.

`_zar` is a little-endian u32 with the decompressed size followed by a single
zlib stream; the stream holds a DAR. A DAR is a u32 entry count followed by,
for each entry: a NUL-terminated name, padding to 4, a u32 size, padding to 16,
`size` bytes of data and one NUL byte. See docs/formats/rlc.md.

The input is untrusted: sizes, offsets, names and the decompressed size are
checked, and errors raise ZarError.

Usage (outputs go to ignored directories such as analysis/):
  python -I profiles/mpo/tools/zar.py list <_zar>
  python -I profiles/mpo/tools/zar.py extract <_zar> <name> <output>
"""

import argparse
import collections
import struct
import sys
import zlib

# The largest _zar on the ULUS-10202 disc decompresses to under 7 MiB; PSP
# user RAM is 24 MiB, so anything bigger is corrupt.
MAX_DECOMPRESSED = 24 * 1024 * 1024
_MAX_ENTRIES = 65536
_MAX_NAME = 256

DarEntry = collections.namedtuple('DarEntry', 'name offset size')


class ZarError(Exception):
  """Raised when a _zar or DAR archive is malformed."""


def _align(value, alignment):
  return (value + alignment - 1) & ~(alignment - 1)


def decompress(data, max_size=MAX_DECOMPRESSED):
  """Returns the DAR stored in a _zar file."""
  if len(data) < 4:
    raise ZarError('_zar is shorter than its size header')
  declared, = struct.unpack_from('<I', data, 0)
  if declared > max_size:
    raise ZarError(f'declared size {declared} exceeds the limit {max_size}')
  inflater = zlib.decompressobj()
  try:
    # One byte more than declared, so an oversized stream is detected without
    # inflating it completely.
    output = inflater.decompress(data[4:], declared + 1)
  except zlib.error as error:
    raise ZarError(f'zlib stream is corrupt: {error}') from error
  if len(output) != declared:
    raise ZarError(f'decompressed {len(output)}+ bytes, header says {declared}')
  if not inflater.eof:
    raise ZarError('zlib stream is truncated')
  if inflater.unused_data:
    raise ZarError(f'{len(inflater.unused_data)} bytes after the zlib stream')
  return output


def _check_name(name, offset):
  if not name:
    raise ZarError(f'empty entry name at 0x{offset:X}')
  if any(c < 0x20 or c > 0x7E for c in name):
    raise ZarError(f'entry name at 0x{offset:X} is not printable ASCII')
  text = name.decode('ascii')
  if '/' in text or '\\' in text or ':' in text or text in ('.', '..'):
    raise ZarError(f'unsafe entry name {text!r}')
  return text


def parse_dar(data):
  """Returns the DarEntry list of a DAR archive held in `data`."""
  if len(data) < 4:
    raise ZarError('DAR is shorter than its entry count')
  count, = struct.unpack_from('<I', data, 0)
  if count > _MAX_ENTRIES:
    raise ZarError(f'DAR declares {count} entries')
  entries = []
  names = set()
  cursor = 4
  for _ in range(count):
    end = data.find(b'\0', cursor, min(len(data), cursor + _MAX_NAME))
    if end < 0:
      raise ZarError(f'entry name at 0x{cursor:X} is not terminated')
    name = _check_name(data[cursor:end], cursor)
    if name in names:
      raise ZarError(f'duplicate entry {name!r}')
    names.add(name)
    size_offset = _align(end + 1, 4)
    if size_offset + 4 > len(data):
      raise ZarError(f'size of {name!r} is out of bounds')
    size, = struct.unpack_from('<I', data, size_offset)
    data_offset = _align(size_offset + 4, 16)
    # Each entry's data is followed by one NUL byte.
    if size > len(data) or data_offset + size + 1 > len(data):
      raise ZarError(f'data of {name!r} is out of bounds')
    if data[data_offset + size] != 0:
      raise ZarError(f'data of {name!r} is not followed by a NUL byte')
    entries.append(DarEntry(name=name, offset=data_offset, size=size))
    cursor = data_offset + size + 1
  if cursor != len(data):
    raise ZarError(
        f'{len(data) - cursor} unexpected bytes after the last entry')
  return entries


def read_entry(data, entries, name):
  """Returns the bytes of entry `name`, or raises ZarError."""
  for entry in entries:
    if entry.name == name:
      return data[entry.offset:entry.offset + entry.size]
  raise ZarError(f'no entry named {name!r}')


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  list_parser = commands.add_parser('list', help='list entry names and sizes')
  list_parser.add_argument('zar')
  extract_parser = commands.add_parser('extract', help='write one entry')
  extract_parser.add_argument('zar')
  extract_parser.add_argument('name')
  extract_parser.add_argument('output')
  args = parser.parse_args(argv)

  with open(args.zar, 'rb') as zar_file:
    raw = zar_file.read()
  try:
    dar = decompress(raw)
    entries = parse_dar(dar)
    if args.command == 'list':
      for entry in entries:
        print(f'{entry.size:10} {entry.name}')
      return 0
    blob = read_entry(dar, entries, args.name)
  except ZarError as error:
    print(f'error: {args.zar}: {error}', file=sys.stderr)
    return 1
  with open(args.output, 'wb') as output_file:
    output_file.write(blob)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
