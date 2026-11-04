#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the inventory of the game's button icons from a texture dump (F25a).

mpo_boot --dump-textures writes every texture in RAM the game draws as
<key>.png (F24c). This tool compares each dumped PNG with the TXP texture
pages listed as [[page]] in config/prompt_icons.toml (decoded with txp.py from
the stages extracted in F04) and prints an [[icon]] entry for every page it
finds: the key, the button and the frame (the page's first TXP frame). The
output goes into <data dir>/prompts/inventory.toml, which mpo_boot merges over
the built-in inventory; the keys (only hex digits and numbers) can also go
into config/prompt_icons.toml.

Only keys and numbers are printed: the dump and the stages are the game's
art and stay in ignored folders (CLAUDE.md §1).

Usage:
  python -I profiles/mpo/tools/prompt_icons.py match <dump dir> <stages dir> \
      [--inventory profiles/mpo/config/prompt_icons.toml] [--out <file>]
  python -I profiles/mpo/tools/prompt_icons.py keys <dump dir>

<stages dir> holds one folder per stage with its cache.qar and resident.qar
(analysis/f04/zar). The dumped PNGs are untrusted input: sizes are checked.
"""

import argparse
import importlib.util
import os
import pathlib
import re
import struct
import sys
import tomllib
import zlib

_TOOLS = pathlib.Path(__file__).resolve().parent
_DEFAULT_INVENTORY = _TOOLS.parent / 'config' / 'prompt_icons.toml'
_KEY_NAME = re.compile(r'^([0-9a-f]{24})\.png$')
_MAX_SIDE = 4096
_BUTTONS = ('cross', 'circle', 'triangle', 'square', 'start', 'select', 'l',
            'r')


def _load_txp():
  # By path, so it also works under `python -I` (no script directory).
  spec = importlib.util.spec_from_file_location('txp', _TOOLS / 'txp.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


txp = _load_txp()


class IconError(Exception):
  pass


def _paeth(a, b, c):
  p = a + b - c
  pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
  if pa <= pb and pa <= pc:
    return a
  return b if pb <= pc else c


def read_png(data):
  """Returns (width, height, RGBA8888 rows) of an 8-bit RGB or RGBA PNG."""
  if data[:8] != b'\x89PNG\r\n\x1a\n':
    raise IconError('not a PNG')
  pos = 8
  header = None
  idat = bytearray()
  while pos + 8 <= len(data):
    length, tag = struct.unpack_from('>I4s', data, pos)
    body = data[pos + 8:pos + 8 + length]
    if len(body) != length:
      raise IconError('truncated chunk')
    pos += 12 + length
    if tag == b'IHDR':
      header = struct.unpack('>IIBBBBB', body)
    elif tag == b'IDAT':
      idat += body
    elif tag == b'IEND':
      break
  if header is None:
    raise IconError('no IHDR')
  width, height, depth, color, _, _, interlace = header
  if not 0 < width <= _MAX_SIDE or not 0 < height <= _MAX_SIDE:
    raise IconError(f'bad size {width}x{height}')
  if depth != 8 or color not in (2, 6) or interlace != 0:
    raise IconError('only 8-bit RGB/RGBA, not interlaced')
  channels = 4 if color == 6 else 3
  stride = width * channels
  try:
    raw = zlib.decompressobj().decompress(bytes(idat), (stride + 1) * height)
  except zlib.error as error:
    raise IconError(f'bad image data: {error}') from error
  if len(raw) < (stride + 1) * height:
    raise IconError('truncated image data')
  out = bytearray()
  previous = bytearray(stride)
  for y in range(height):
    kind = raw[y * (stride + 1)]
    line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for i in range(stride):
      left = line[i - channels] if i >= channels else 0
      up = previous[i]
      corner = previous[i - channels] if i >= channels else 0
      if kind == 1:
        line[i] = (line[i] + left) & 0xFF
      elif kind == 2:
        line[i] = (line[i] + up) & 0xFF
      elif kind == 3:
        line[i] = (line[i] + (left + up) // 2) & 0xFF
      elif kind == 4:
        line[i] = (line[i] + _paeth(left, up, corner)) & 0xFF
      elif kind != 0:
        raise IconError(f'bad filter {kind}')
    previous = line
    if channels == 4:
      out += line
    else:
      for x in range(width):
        out += line[x * 3:x * 3 + 3] + b'\xff'
  return width, height, bytes(out)


def _crop(width, height, pixels, new_width, new_height):
  rows = []
  for y in range(new_height):
    rows.append(pixels[y * width * 4:y * width * 4 + new_width * 4])
  return b''.join(rows)


def dumped(dump_dir):
  """Yields (name without .png, width, height, RGBA) of each dumped key."""
  for name in sorted(os.listdir(dump_dir)):
    match = _KEY_NAME.match(name)
    if not match:
      continue
    path = os.path.join(dump_dir, name)
    with open(path, 'rb') as png:
      data = png.read()
    try:
      width, height, pixels = read_png(data)
    except IconError as error:
      print(f'warning: {name}: {error}', file=sys.stderr)
      continue
    yield match.group(1), width, height, pixels


def _pages(inventory_path):
  with open(inventory_path, 'rb') as toml_file:
    inventory = tomllib.load(toml_file)
  pages = []
  for i, page in enumerate(inventory.get('page', [])):
    name, texture, button = (page.get('txp'), page.get('texture'),
                             page.get('button'))
    if (not isinstance(name, str) or not isinstance(texture, int) or
        button not in _BUTTONS):
      raise IconError(f'[[page]] {i + 1} needs txp, texture and a button')
    pages.append((name, texture, button))
  known = {icon.get('key') for icon in inventory.get('icon', [])}
  return pages, known


def _find_txp(stages_dir, name):
  """The first TXP called `name` in the stages, with where it was."""
  for stage in sorted(os.listdir(stages_dir)):
    for archive in ('resident.qar', 'cache.qar'):
      path = os.path.join(stages_dir, stage, archive)
      if not os.path.isfile(path):
        continue
      with open(path, 'rb') as qar_file:
        data = qar_file.read()
      for entry in txp.parse_qar(data):
        if entry.name == name:
          blob = data[entry.offset:entry.offset + entry.size]
          return txp.parse_txp(blob), f'{stage}/{archive}'
  return None, None


def match(dump_dir, stages_dir, inventory_path):
  """Returns the [[icon]] TOML entries for the inventory's pages."""
  pages, known = _pages(inventory_path)
  dump = list(dumped(dump_dir))
  entries = []
  missing = []
  for name, index, button in pages:
    parsed, where = _find_txp(stages_dir, name)
    if parsed is None or not 0 <= index < len(parsed.textures):
      missing.append(f'{name} texture {index} (not in the stages)')
      continue
    texture = parsed.textures[index]
    page = txp.rgba(parsed, texture, 0)
    frame = next((f for f in parsed.frames if f.texture == index), None)
    found = False
    for key, width, height, pixels in dump:
      w = min(width, texture.buf_width)
      h = min(height, texture.height)
      if (w == 0 or h == 0 or width > texture.pot_width or
          height > texture.pot_height):
        continue
      if (_crop(width, height, pixels, w, h) !=
          _crop(texture.buf_width, texture.height, page, w, h)):
        continue
      found = True
      if key in known:
        continue
      known.add(key)
      lines = ['[[icon]]', f'key = "{key}"', f'button = "{button}"']
      if frame is not None and frame.width and frame.height:
        lines.append(f'frame = [{frame.x}, {frame.y}, {frame.width}, '
                     f'{frame.height}]')
      lines.append(f'source = "{name} texture {index} ({where})"')
      entries.append('\n'.join(lines) + '\n')
    if not found:
      missing.append(f'{name} texture {index} (not in the dump: show it in '
                     'the game while dumping)')
  return entries, missing


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  match_parser = commands.add_parser('match', help='dump -> [[icon]] entries')
  match_parser.add_argument('dump')
  match_parser.add_argument('stages')
  match_parser.add_argument('--inventory', default=str(_DEFAULT_INVENTORY))
  match_parser.add_argument('--out')
  keys_parser = commands.add_parser('keys', help='list the dumped keys')
  keys_parser.add_argument('dump')
  args = parser.parse_args(argv)
  try:
    if args.command == 'keys':
      for key, width, height, _ in dumped(args.dump):
        print(f'{key} {width}x{height}')
      return 0
    entries, missing = match(args.dump, args.stages, args.inventory)
  except (IconError, txp.TxpError, OSError, tomllib.TOMLDecodeError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 1
  text = ''.join(f'\n{e}' for e in entries)
  if args.out:
    with open(args.out, 'a', encoding='utf-8') as out:
      out.write(text)
  else:
    sys.stdout.write(text)
  for line in missing:
    print(f'missing: {line}', file=sys.stderr)
  print(f'{len(entries)} new icons, {len(missing)} pages not found',
        file=sys.stderr)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
