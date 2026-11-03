#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads QAR archives and the TXP textures inside them (read-only analysis).

A QAR (`cache.qar`, `resident.qar` in a stage's DAR) ends with a u32 offset to
its list: a u32 count, `count` pairs of (u32 resource hash, u32 size) and the
NUL-terminated names; the data of each entry starts at 0, aligned to 0x80.

A TXP starts with eight u32 (link, flags, texture count, texture table, frame
count, frame table, 2 x reserved). A texture (0x10 bytes) is u16 width (low 12
bits buffer width, high 4 log2 width), u16 height (same), u16 GE format, u16
flags (bit 1: zlib), u32 pixel offset, u32 CLUT offset. A frame (0x30 bytes) is
u32 flags, u32 hash, u32 runtime, u16 w, h, x, y, u32 texture offset, u16
palette, u16 pad, 4 x f32 (uv offset and scale), u32 pad. The pixels are
swizzled (16-byte x 8-row blocks); CLUTs are RGBA8888, 16 entries per palette
step. Layout from waspothegreat/MGS-MPO-Noesis, checked against the disc.

The input is untrusted: every size and offset is checked and errors raise
TxpError.

Usage (outputs go to ignored directories such as analysis/):
  python -I profiles/mpo/tools/txp.py qar <qar>
  python -I profiles/mpo/tools/txp.py info <qar> <txp name>
  python -I profiles/mpo/tools/txp.py png <qar> <txp name> <texture> <out.png>
  python -I profiles/mpo/tools/txp.py sheet <qar> <txp name> <out.png>
  python -I profiles/mpo/tools/txp.py inventory <directory of extracted stages>
"""

import argparse
import collections
import hashlib
import os
import struct
import sys
import zlib

_MAX_ENTRIES = 65536
_MAX_TEXTURES = 4096
_MAX_FRAMES = 65536
_MAX_SIDE = 4096

# Bits per texel of the GE texture formats.
_BPP = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8, 6: 16, 7: 32}
FORMAT_NAMES = {0: '5650', 1: '5551', 2: '4444', 3: '8888', 4: 'T4', 5: 'T8',
                6: 'T16', 7: 'T32'}

QarEntry = collections.namedtuple('QarEntry', 'name hash offset size')
Texture = collections.namedtuple(
    'Texture', 'index buf_width height pot_width pot_height fmt flags pixels '
    'clut')
Frame = collections.namedtuple('Frame',
                               'index flags hash width height x y texture '
                               'palette')
Txp = collections.namedtuple('Txp', 'flags textures frames data')


class TxpError(Exception):
  """Raised when a QAR or TXP is malformed."""


def strcode24(name):
  """Returns Kojima's 24-bit strcode of a byte string."""
  value = 0
  for c in name:
    value = ((value >> 19) | (value << 5)) & 0xFFFFFFFF
    value = (value + c) & 0xFFFFFF
  return value or 1


def _align(value, alignment):
  return (value + alignment - 1) & ~(alignment - 1)


def parse_qar(data):
  """Returns the QarEntry list of a QAR archive."""
  if len(data) < 8:
    raise TxpError('QAR is too short')
  list_offset, = struct.unpack_from('<I', data, len(data) - 4)
  if list_offset + 4 > len(data) - 4:
    raise TxpError(f'list offset 0x{list_offset:X} is out of the archive')
  count, = struct.unpack_from('<I', data, list_offset)
  if count > _MAX_ENTRIES or list_offset + 4 + count * 8 > len(data):
    raise TxpError(f'bad entry count {count}')
  pairs = [struct.unpack_from('<II', data, list_offset + 4 + 8 * i)
           for i in range(count)]
  cursor = list_offset + 4 + count * 8
  position = 0
  entries = []
  for resource_hash, size in pairs:
    end = data.find(b'\0', cursor, min(len(data), cursor + 130))
    if end < 0:
      raise TxpError(f'unterminated name at 0x{cursor:X}')
    raw_name = data[cursor:end]
    if not raw_name or any(c < 0x21 or c > 0x7E or c in b'/\\:'
                           for c in raw_name):
      raise TxpError(f'bad entry name at 0x{cursor:X}')
    cursor = end + 1
    if position + size > list_offset:
      raise TxpError(f'entry {raw_name!r} runs past the list')
    entries.append(QarEntry(raw_name.decode('ascii'), resource_hash, position,
                            size))
    position = _align(position + size, 0x80)
  return entries


def parse_txp(data):
  """Returns the Txp held in `data`."""
  if len(data) < 0x20:
    raise TxpError('TXP is shorter than its header')
  link, flags, n_tex, tex_offset, n_frames, frame_offset, _, _ = (
      struct.unpack_from('<8I', data, 0))
  if link != 0 or not 0 < n_tex <= _MAX_TEXTURES or n_frames > _MAX_FRAMES:
    raise TxpError('bad TXP header')
  if tex_offset < 0x20 or tex_offset + n_tex * 0x10 > len(data):
    raise TxpError('texture table is out of the file')
  if n_frames and (frame_offset < 0x20 or
                   frame_offset + n_frames * 0x30 > len(data)):
    raise TxpError('frame table is out of the file')
  textures = []
  for i in range(n_tex):
    width, height, fmt, tex_flags, pixels, clut = struct.unpack_from(
        '<HHHHII', data, tex_offset + 0x10 * i)
    texture = Texture(i, width & 0xFFF, height & 0xFFF, 1 << (width >> 12),
                      1 << (height >> 12), fmt, tex_flags, pixels, clut)
    if (fmt not in _BPP or not 0 < texture.buf_width <= _MAX_SIDE or
        not 0 < texture.height <= _MAX_SIDE or pixels >= len(data) or
        (fmt >= 4 and clut >= len(data))):
      raise TxpError(f'bad texture {i}')
    textures.append(texture)
  frames = []
  for i in range(n_frames):
    (frame_flags, frame_hash, _, width, height, x, y, texture_offset,
     palette, _) = struct.unpack_from('<IIIHHHHIHH', data,
                                      frame_offset + 0x30 * i)
    index, rest = divmod(texture_offset - tex_offset, 0x10)
    if texture_offset < tex_offset or rest or index >= n_tex:
      raise TxpError(f'frame {i} points to no texture')
    frames.append(Frame(i, frame_flags, frame_hash, width, height, x, y,
                        index, palette))
  return Txp(flags, textures, frames, data)


def _unswizzle(raw, row_bytes, height):
  out = bytearray(len(raw))
  blocks_per_row = row_bytes // 16
  source = 0
  for block_y in range(0, height, 8):
    for block_x in range(blocks_per_row):
      target = block_y * row_bytes + block_x * 16
      for _ in range(8):
        out[target:target + 16] = raw[source:source + 16]
        source += 16
        target += row_bytes
  return bytes(out)


def texels(txp, texture):
  """Returns the unswizzled texels of a texture and its row size in bytes."""
  row_bytes = texture.buf_width * _BPP[texture.fmt] // 8
  size = row_bytes * texture.height
  data = txp.data
  if texture.flags & 2:
    if texture.pixels + 4 > len(data):
      raise TxpError('compressed texture header is out of the file')
    packed = struct.unpack_from('<I', data, texture.pixels)[0] & 0xFFFFFF
    stream = data[texture.pixels + 4:texture.pixels + 4 + packed]
    inflater = zlib.decompressobj()
    try:
      raw = inflater.decompress(stream, size)
    except zlib.error as error:
      raise TxpError(f'texture {texture.index}: {error}') from error
  else:
    raw = data[texture.pixels:texture.pixels + size]
  if len(raw) < size:
    raise TxpError(f'texture {texture.index} is truncated')
  if row_bytes % 16 == 0 and texture.height % 8 == 0:
    raw = _unswizzle(raw, row_bytes, texture.height)
  return raw, row_bytes


def rgba(txp, texture, palette=0):
  """Decodes a whole texture page to RGBA8888 rows."""
  raw, _ = texels(txp, texture)
  width, height = texture.buf_width, texture.height
  if texture.fmt in (4, 5):
    entries = 16 if texture.fmt == 4 else 256
    start = texture.clut + palette * 0x40
    clut = txp.data[start:start + entries * 4].ljust(entries * 4, b'\0')
    colors = [clut[i * 4:i * 4 + 4] for i in range(entries)]
    if texture.fmt == 4:
      indices = []
      for byte in raw:
        indices.append(byte & 15)
        indices.append(byte >> 4)
    else:
      indices = raw
    return b''.join(colors[i] for i in indices[:width * height])
  if texture.fmt == 3:
    return raw[:width * height * 4]
  out = bytearray()
  for i in range(width * height):
    value = raw[2 * i] | raw[2 * i + 1] << 8
    if texture.fmt == 0:
      out += bytes(((value & 31) * 255 // 31, (value >> 5 & 63) * 255 // 63,
                    (value >> 11 & 31) * 255 // 31, 255))
    elif texture.fmt == 1:
      out += bytes(((value & 31) * 255 // 31, (value >> 5 & 31) * 255 // 31,
                    (value >> 10 & 31) * 255 // 31,
                    255 if value & 0x8000 else 0))
    else:
      out += bytes(((value & 15) * 17, (value >> 4 & 15) * 17,
                    (value >> 8 & 15) * 17, (value >> 12 & 15) * 17))
  return bytes(out)


def write_png(path, width, height, pixels):
  """Writes RGBA8888 rows as a PNG file."""
  def chunk(tag, body):
    return (struct.pack('>I', len(body)) + tag + body +
            struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF))
  rows = b''.join(b'\0' + pixels[y * width * 4:(y + 1) * width * 4]
                  for y in range(height))
  with open(path, 'wb') as png:
    png.write(b'\x89PNG\r\n\x1a\n')
    png.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0,
                                         0, 0)))
    png.write(chunk(b'IDAT', zlib.compress(rows, 9)))
    png.write(chunk(b'IEND', b''))


def contact_sheet(txp, scale=2):
  """Stacks every texture page over gray; returns (width, height, RGBA)."""
  pages = []
  for texture in txp.textures:
    try:
      pixels = rgba(txp, texture, 0)
    except TxpError:
      continue
    pages.append((texture.buf_width, texture.height, pixels))
  width = max(w for w, _, _ in pages) * scale
  height = sum((h + 2) * scale for _, h, _ in pages)
  out = bytearray(bytes((40, 40, 70, 255)) * (width * height))
  top = 0
  for page_width, page_height, pixels in pages:
    for y in range(page_height * scale):
      for x in range(page_width * scale):
        source = ((y // scale) * page_width + x // scale) * 4
        r, g, b, a = pixels[source:source + 4]
        # The PSP alpha in CLUTs is 0..0x80 for many HUD textures.
        a = min(255, a * 2)
        target = ((top + y) * width + x) * 4
        out[target:target + 4] = bytes(
            ((r * a + 96 * (255 - a)) // 255, (g * a + 96 * (255 - a)) // 255,
             (b * a + 96 * (255 - a)) // 255, 255))
    top += (page_height + 2) * scale
  return width, height, bytes(out)


def _read_entry(path, name):
  with open(path, 'rb') as qar_file:
    data = qar_file.read()
  for entry in parse_qar(data):
    if entry.name == name:
      return data[entry.offset:entry.offset + entry.size]
  raise TxpError(f'{path} has no entry {name!r}')


def _inventory(root):
  """Prints every TXP of every stage QAR and how often each one recurs."""
  seen = collections.defaultdict(set)
  for stage in sorted(os.listdir(root)):
    for archive in ('cache.qar', 'resident.qar'):
      path = os.path.join(root, stage, archive)
      if not os.path.isfile(path):
        continue
      with open(path, 'rb') as qar_file:
        data = qar_file.read()
      for entry in parse_qar(data):
        if not entry.name.endswith('.txp'):
          continue
        blob = data[entry.offset:entry.offset + entry.size]
        digest = hashlib.sha256(blob).hexdigest()[:16]
        seen[(entry.name, digest)].add(f'{stage}/{archive}')
  print('stages  sha256[:16]       txp')
  for (name, digest), where in sorted(seen.items(),
                                      key=lambda item: (item[0][0],
                                                        -len(item[1]))):
    print(f'{len(where):6}  {digest}  {name}')
  return 0


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  qar_parser = commands.add_parser('qar', help='list QAR entries')
  qar_parser.add_argument('qar')
  info_parser = commands.add_parser('info', help='textures and frames')
  info_parser.add_argument('qar')
  info_parser.add_argument('name')
  png_parser = commands.add_parser('png', help='decode one texture page')
  png_parser.add_argument('qar')
  png_parser.add_argument('name')
  png_parser.add_argument('texture', type=int)
  png_parser.add_argument('output')
  png_parser.add_argument('--palette', type=int, default=0)
  sheet_parser = commands.add_parser('sheet', help='all pages in one PNG')
  sheet_parser.add_argument('qar')
  sheet_parser.add_argument('name')
  sheet_parser.add_argument('output')
  inventory_parser = commands.add_parser('inventory',
                                         help='TXP recurrence by stage')
  inventory_parser.add_argument('root')
  args = parser.parse_args(argv)

  try:
    if args.command == 'inventory':
      return _inventory(args.root)
    if args.command == 'qar':
      with open(args.qar, 'rb') as qar_file:
        for entry in parse_qar(qar_file.read()):
          print(f'{entry.hash:08X} {entry.size:8} {entry.name}')
      return 0
    txp = parse_txp(_read_entry(args.qar, args.name))
    if args.command == 'info':
      print(f'flags {txp.flags:08X}, {len(txp.textures)} textures, '
            f'{len(txp.frames)} frames')
      for t in txp.textures:
        print(f'texture {t.index}: {t.buf_width}x{t.height} '
              f'(pot {t.pot_width}x{t.pot_height}) '
              f'{FORMAT_NAMES[t.fmt]} flags {t.flags:04X} '
              f'pixels 0x{t.pixels:X} clut 0x{t.clut:X}')
      for f in txp.frames:
        print(f'frame {f.index}: hash {f.hash:08X} texture {f.texture} '
              f'pal {f.palette} {f.width}x{f.height} at {f.x},{f.y} '
              f'flags {f.flags:04X}')
      return 0
    if args.command == 'sheet':
      write_png(args.output, *contact_sheet(txp))
      return 0
    if not 0 <= args.texture < len(txp.textures):
      raise TxpError(f'no texture {args.texture}')
    texture = txp.textures[args.texture]
    write_png(args.output, texture.buf_width, texture.height,
              rgba(txp, texture, args.palette))
    return 0
  except TxpError as error:
    print(f'error: {error}', file=sys.stderr)
    return 1


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
