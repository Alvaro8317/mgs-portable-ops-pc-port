#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds HD texture packs from a texture dump (F26b).

mpo_boot --dump-textures <dump> writes every texture the game draws as
<key>.png, a textures.ini and a dump.csv with, per key, its size, format,
RAM or VRAM, the first stage it was drawn in, the stages, the draws and the
frames (several sessions into the same folder add up). This tool reads it:

  report   the textures by use, by stage or by size, to know where to start
           (the walls and floors seen the most):
             texture_pack.py report <dump> [--sort uses|frames|size|stage]
                 [--stage S] [--limit N] [--keys-only]
  upscale  scales the dumped PNGs with a scaler the player installs and
           writes them, with the same names and a textures.ini, into a pack:
             texture_pack.py upscale <dump> <pack> [--scale 4]
                 [--realesrgan EXE] [--model realesrgan-x4plus]
                 [--models DIR] [--alpha scaler|nearest]
                 [--stage S] [--min-uses N] [--top N] [--keys K ...]
                 [--overwrite]
           or with any other command, run once per image:
             texture_pack.py upscale <dump> <pack> --scale 2 -- \\
                 <command> ... {input} ... {output} ... {scale}
  check    validates a pack against its dump: every PNG has a key of the
           dump, the same aspect and a size that is a whole multiple of the
           original (at most 4096), file names and textures.ini:
             texture_pack.py check <pack> <dump>

The scaler sees the colour (RGB) and, when the texture is not opaque, the
alpha as a grey image of its own, so a scaler that drops the alpha channel
(as most do) keeps it anyway; an alpha that was only 0 and 255 (alpha-tested
cut-outs) stays only 0 and 255. --alpha nearest scales the alpha by repeating
pixels instead.

Run it with `python -I` (Python 3.11 or later, standard library only). The
dump and every pack made from it are the game's art: they stay on the
player's disk and never go into the repository (CLAUDE.md §1). The repo
ships no scaler and no model: realesrgan-ncnn-vulkan
(https://github.com/xinntao/Real-ESRGAN, BSD-3-Clause) and its models are
installed by the player.

Every file read is untrusted input: dump.csv rows, PNG chunks, sizes and the
paths in textures.ini are checked. Exit status: 0 on success, 1 when
something failed (check: when the pack has errors), 2 on bad arguments.
"""

import argparse
import dataclasses
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import zlib

_TOOLS = pathlib.Path(__file__).resolve().parent
_BUILD_ORDER = _TOOLS.parent / 'config' / 'build_order.toml'

DUMP_FILE = 'dump.csv'
DUMP_HEADER = 'key,width,height,format,memory,first_stage,stages,uses,frames'
FORMATS = ('5650', '5551', '4444', '8888', 'CLUT4', 'CLUT8', 'CLUT16',
           'CLUT32', 'DXT1', 'DXT3', 'DXT5')
MEMORIES = ('ram', 'vram', 'ram+vram')
# The runtime's limits (texture_replacer.hpp kMaxImageSide, dump_index.hpp).
MAX_PACK_SIDE = 4096
MAX_DUMP_BYTES = 64 << 20
MAX_STAGES = 128
MAX_COUNT = 1 << 62
# Larger images are refused before decoding (a scaler's output included).
MAX_READ_SIDE = 8192
MAX_PNG_BYTES = 256 << 20

_KEY = re.compile(r'^[0-9a-f]{24}$')
_STAGE = re.compile(r'^[A-Za-z0-9_.-]{1,64}$')
_HASH_NAME = re.compile(r'^([0-9A-Fa-f]{24})(?:_([0-9]+))?$')
_SIGNATURE = b'\x89PNG\r\n\x1a\n'

# DefaultTexturesIni() of host/texreplace/textures_ini.cpp.
DEFAULT_TEXTURES_INI = (
    '# Texture replacement for MGS: Portable Ops (ULUS-10202), written by '
    'mpo_boot\n'
    '# --dump-textures. Same format as PPSSPP\'s textures.ini:\n'
    '# https://www.ppsspp.org/docs/reference/texture-replacement\n'
    '# The dumped PNGs are named by their key; an edited PNG with that name, '
    'or\n'
    '# any PNG listed under [hashes], replaces the texture.\n'
    '\n'
    '[options]\n'
    'version = 1\n'
    'hash = xxh64\n'
    'ignoreAddress = true\n'
    '\n'
    '[hashes]\n'
    '# 00000000a1b2c3d4e5f60718 = icons/triangle.png\n')


class PackError(Exception):
  """A file or an argument this tool cannot use."""


# ---- PNG ----


def _chunks(data):
  """Yields (type, body) of each chunk, checking lengths and CRCs."""
  if not data.startswith(_SIGNATURE):
    raise PackError('not a PNG file')
  pos = len(_SIGNATURE)
  while True:
    if pos + 12 > len(data):
      raise PackError('truncated PNG (no IEND)')
    length, kind = struct.unpack_from('>I4s', data, pos)
    if length > len(data) - pos - 12:
      raise PackError(f'chunk {kind!r} is truncated')
    body = data[pos + 8:pos + 8 + length]
    (crc,) = struct.unpack_from('>I', data, pos + 8 + length)
    if zlib.crc32(kind + body) != crc:
      raise PackError(f'chunk {kind!r} has a bad CRC')
    pos += 12 + length
    yield kind, body
    if kind == b'IEND':
      return


_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
_DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16),
           6: (8, 16)}


def png_header(data):
  """Returns (width, height, depth, colour type, interlace) of a PNG.

  Only the signature and the first chunk (IHDR) are read.
  """
  if not data.startswith(_SIGNATURE) or len(data) < 33:
    raise PackError('not a PNG file')
  length, kind = struct.unpack_from('>I4s', data, 8)
  if kind != b'IHDR' or length != 13:
    raise PackError('the first chunk is not IHDR')
  body = data[16:29]
  (crc,) = struct.unpack_from('>I', data, 29)
  if zlib.crc32(kind + body) != crc:
    raise PackError('IHDR has a bad CRC')
  width, height, depth, color, compression, filtering, interlace = (
      struct.unpack('>IIBBBBB', body))
  if color not in _CHANNELS or depth not in _DEPTHS[color]:
    raise PackError(f'bad colour type {color} with depth {depth}')
  if compression != 0 or filtering != 0 or interlace not in (0, 1):
    raise PackError('bad IHDR')
  if width == 0 or height == 0:
    raise PackError('empty image')
  return width, height, depth, color, interlace


def _paeth(a, b, c):
  p = a + b - c
  pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
  if pa <= pb and pa <= pc:
    return a
  return b if pb <= pc else c


def _unfilter(raw, stride, height, bpp):
  out = bytearray(stride * height)
  prev = bytes(stride)
  pos = 0
  for y in range(height):
    kind = raw[pos]
    line = bytearray(raw[pos + 1:pos + 1 + stride])
    pos += stride + 1
    if kind == 1:
      for i in range(bpp, stride):
        line[i] = (line[i] + line[i - bpp]) & 0xFF
    elif kind == 2:
      line = bytearray((a + b) & 0xFF for a, b in zip(line, prev))
    elif kind == 3:
      for i in range(stride):
        left = line[i - bpp] if i >= bpp else 0
        line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
    elif kind == 4:
      for i in range(stride):
        if i >= bpp:
          line[i] = (line[i] + _paeth(line[i - bpp], prev[i],
                                      prev[i - bpp])) & 0xFF
        else:
          line[i] = (line[i] + prev[i]) & 0xFF
    elif kind != 0:
      raise PackError(f'unknown filter type {kind} on row {y}')
    out[y * stride:(y + 1) * stride] = line
    prev = line
  return out


def read_png(data, max_side=MAX_READ_SIDE):
  """Decodes a PNG (any colour type, 1 to 16 bits, not interlaced).

  Returns (width, height, RGBA8888 bytes).
  """
  width, height, depth, color, interlace = png_header(data)
  if width > max_side or height > max_side:
    raise PackError(f'{width}x{height} is larger than {max_side}')
  if interlace:
    raise PackError('interlaced PNGs are not supported')
  palette = None
  transparency = None
  idat = []
  for kind, body in _chunks(data):
    if kind == b'PLTE':
      if len(body) % 3 or not body:
        raise PackError('bad PLTE')
      palette = body
    elif kind == b'tRNS':
      transparency = body
    elif kind == b'IDAT':
      idat.append(body)
  channels = _CHANNELS[color]
  bits = channels * depth
  stride = (width * bits + 7) // 8
  bpp = max(1, bits // 8)
  expected = (stride + 1) * height
  inflater = zlib.decompressobj()
  try:
    raw = inflater.decompress(b''.join(idat), expected)
  except zlib.error as error:
    raise PackError(f'corrupt image data: {error}') from error
  if len(raw) != expected:
    raise PackError(f'image data holds {len(raw)} bytes, expected {expected}')
  pixels = _unfilter(raw, stride, height, bpp)
  count = width * height
  if depth == 16:
    pixels = pixels[0::2]  # The high byte of each sample.
  elif depth < 8:
    # Gray or palette indices of 1, 2 or 4 bits, rows padded to a byte.
    unpacked = bytearray(count)
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    for y in range(height):
      row = pixels[y * stride:(y + 1) * stride]
      base = y * width
      for x in range(width):
        byte = row[x // per_byte]
        shift = 8 - depth * (x % per_byte + 1)
        unpacked[base + x] = (byte >> shift) & mask
    pixels = unpacked
  rgba = bytearray(count * 4)
  if color == 3:
    if palette is None:
      raise PackError('palette image without PLTE')
    entries = len(palette) // 3
    alphas = bytes(transparency or b'')[:entries]
    table = []
    for i in range(256):
      if i < entries:
        a = alphas[i] if i < len(alphas) else 255
        table.append(palette[i * 3:i * 3 + 3] + bytes((a,)))
      else:
        table.append(None)
    out = bytearray()
    for index in pixels:
      entry = table[index]
      if entry is None:
        raise PackError(f'palette index {index} out of range')
      out += entry
    return width, height, bytes(out)
  if color in (0, 4):
    if depth < 8:
      scale = 255 // ((1 << depth) - 1)
      gray = bytes(v * scale for v in pixels)
    else:
      gray = bytes(pixels[0::channels])
    rgba[0::4] = gray
    rgba[1::4] = gray
    rgba[2::4] = gray
    rgba[3::4] = (bytes(pixels[1::2]) if color == 4 else b'\xff' * count)
    if color == 0 and transparency is not None and len(transparency) == 2:
      key = struct.unpack('>H', transparency)[0]
      if depth == 16:
        key >>= 8
      elif depth < 8:
        key *= 255 // ((1 << depth) - 1)
      for i, value in enumerate(gray):
        if value == key:
          rgba[i * 4 + 3] = 0
    return width, height, bytes(rgba)
  # RGB or RGBA.
  for c in range(3):
    rgba[c::4] = pixels[c::channels]
  if color == 6:
    rgba[3::4] = pixels[3::4]
  else:
    rgba[3::4] = b'\xff' * count
    if transparency is not None and len(transparency) == 6:
      key = struct.unpack('>HHH', transparency)
      if depth == 16:
        key = tuple(k >> 8 for k in key)
      key = bytes(key)
      for i in range(count):
        if rgba[i * 4:i * 4 + 3] == key:
          rgba[i * 4 + 3] = 0
  return width, height, bytes(rgba)


def encode_png(width, height, pixels, channels=4):
  """Encodes 8-bit pixels (1 grey, 3 RGB or 4 RGBA channels) as a PNG."""
  color = {1: 0, 3: 2, 4: 6}[channels]
  stride = width * channels
  if len(pixels) != stride * height:
    raise PackError('pixel buffer does not match the size')
  rows = bytearray()
  prev = bytes(stride)
  for y in range(height):
    line = bytes(pixels[y * stride:(y + 1) * stride])
    # Sub or Up, whichever looks smaller (the usual heuristic).
    sub = line[:channels] + bytes(
        (a - b) & 0xFF for a, b in zip(line[channels:], line))
    up = bytes((a - b) & 0xFF for a, b in zip(line, prev))
    cost_sub = sum(min(v, 256 - v) for v in sub)
    cost_up = sum(min(v, 256 - v) for v in up)
    if cost_up < cost_sub:
      rows += b'\x02' + up
    else:
      rows += b'\x01' + sub
    prev = line

  def chunk(kind, body):
    return (struct.pack('>I', len(body)) + kind + body +
            struct.pack('>I', zlib.crc32(kind + body)))

  return b''.join((
      _SIGNATURE,
      chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0, 0,
                                 0)),
      chunk(b'IDAT', zlib.compress(bytes(rows), 6)),
      chunk(b'IEND', b'')))


def _read_file(path, limit=MAX_PNG_BYTES):
  size = os.path.getsize(path)
  if size > limit:
    raise PackError(f'{path}: larger than {limit >> 20} MiB')
  with open(path, 'rb') as stream:
    return stream.read(limit + 1)


def load_png(path, max_side=MAX_READ_SIDE):
  try:
    return read_png(_read_file(path), max_side)
  except PackError as error:
    raise PackError(f'{path}: {error}') from error


def save_png(path, width, height, pixels, channels=4):
  data = encode_png(width, height, pixels, channels)
  tmp = f'{path}.tmp'
  with open(tmp, 'wb') as out:
    out.write(data)
  os.replace(tmp, path)


def check_png_data(data):
  """Checks every chunk and that the image data inflates to its size.

  Returns (width, height). Cheaper than read_png: no unfiltering.
  """
  width, height, depth, color, interlace = png_header(data)
  if width > MAX_READ_SIDE or height > MAX_READ_SIDE:
    raise PackError(f'{width}x{height} is larger than {MAX_READ_SIDE}')
  idat = []
  for kind, body in _chunks(data):
    if kind == b'IDAT':
      idat.append(body)
  bits = _CHANNELS[color] * depth
  if interlace:
    expected = None
  else:
    expected = ((width * bits + 7) // 8 + 1) * height
  inflater = zlib.decompressobj()
  try:
    raw = inflater.decompress(b''.join(idat),
                              (expected or MAX_PNG_BYTES) + 1)
  except zlib.error as error:
    raise PackError(f'corrupt image data: {error}') from error
  if expected is not None and len(raw) != expected:
    raise PackError(f'image data holds {len(raw)} bytes, expected {expected}')
  if not inflater.eof:
    raise PackError('image data does not end')
  return width, height


# ---- dump.csv ----


@dataclasses.dataclass
class Texture:
  """One key of a dump."""
  key: str
  width: int
  height: int
  format: str = ''
  memory: str = ''
  first_stage: str = ''
  stages: tuple = ()
  uses: int = 0
  frames: int = 0
  in_csv: bool = False
  png: bool = False


def _parse_count(text, maximum):
  if not text.isdigit() or not text.isascii() or len(text) > 20:
    raise ValueError
  value = int(text)
  if value > maximum:
    raise ValueError
  return value


def parse_dump_csv(text):
  """Parses the text of a dump.csv. Returns {key: Texture}.

  The same rules as DumpIndex::Parse (host/texreplace/dump_index.cpp).
  """
  textures = {}
  header = False
  for number, line in enumerate(text.split('\n'), 1):
    if line.endswith('\r'):
      line = line[:-1]
    if not line:
      continue
    where = f'line {number}: '
    if not header:
      if line != DUMP_HEADER:
        raise PackError(f'{where}the header is not "{DUMP_HEADER}"')
      header = True
      continue
    fields = line.split(',')
    if len(fields) != 9:
      raise PackError(f'{where}{len(fields)} fields, not 9')
    key, width, height, fmt, memory, first, stages, uses, frames = fields
    if not _KEY.match(key):
      raise PackError(f'{where}bad key')
    try:
      width = _parse_count(width, MAX_PACK_SIDE)
      height = _parse_count(height, MAX_PACK_SIDE)
      if width == 0 or height == 0:
        raise ValueError
    except ValueError:
      raise PackError(f'{where}bad size') from None
    if fmt not in FORMATS:
      raise PackError(f'{where}bad format')
    if memory not in MEMORIES:
      raise PackError(f'{where}bad memory')
    if first and not _STAGE.match(first):
      raise PackError(f'{where}bad first stage')
    stage_list = stages.split(';') if stages else []
    if (len(stage_list) > MAX_STAGES or
        len(set(stage_list)) != len(stage_list) or
        not all(_STAGE.match(s) for s in stage_list)):
      raise PackError(f'{where}bad stage list')
    try:
      uses = _parse_count(uses, MAX_COUNT)
      frames = _parse_count(frames, MAX_COUNT)
    except ValueError:
      raise PackError(f'{where}bad count') from None
    if key in textures:
      raise PackError(f'{where}the key is listed twice')
    textures[key] = Texture(key, width, height, fmt, memory, first,
                            tuple(stage_list), uses, frames, in_csv=True)
  return textures


def load_dump(dump_dir):
  """Reads a dump folder: its dump.csv and the PNGs named by key.

  A PNG with no row gets its size from the PNG (an older dump, or a
  dump.csv not written yet); a row with no PNG keeps png=False.
  Returns (textures {key: Texture}, has_csv).
  """
  if not os.path.isdir(dump_dir):
    raise PackError(f'{dump_dir}: not a folder')
  path = os.path.join(dump_dir, DUMP_FILE)
  textures = {}
  has_csv = os.path.isfile(path)
  if has_csv:
    raw = _read_file(path, MAX_DUMP_BYTES)
    try:
      textures = parse_dump_csv(raw.decode('ascii'))
    except UnicodeDecodeError:
      raise PackError(f'{path}: not a text file') from None
    except PackError as error:
      raise PackError(f'{path}: {error}') from error
  for name in sorted(os.listdir(dump_dir)):
    stem, ext = os.path.splitext(name)
    if ext != '.png' or not _KEY.match(stem):
      continue
    texture = textures.get(stem)
    if texture is None:
      try:
        width, height, _, _, _ = png_header(
            _read_file(os.path.join(dump_dir, name)))
      except PackError as error:
        print(f'warning: {name}: {error}', file=sys.stderr)
        continue
      texture = Texture(stem, width, height)
      textures[stem] = texture
    texture.png = True
  return textures, has_csv


# ---- report ----


def story_order(path=_BUILD_ORDER):
  """The stages in the order the game meets them (config/build_order.toml)."""
  try:
    with open(path, 'rb') as stream:
      first = tomllib.load(stream).get('first', [])
  except (OSError, tomllib.TOMLDecodeError):
    return []
  return [s for s in first if isinstance(s, str)]


def _stage_rank(order):
  ranks = {name: i for i, name in enumerate(order)}

  def rank(stage):
    if not stage:
      return (0, 0, '')
    if stage in ranks:
      return (1, ranks[stage], stage)
    return (2, 0, stage)

  return rank


def select(textures, stage=None, min_uses=0):
  out = []
  for t in textures.values():
    if stage is not None and stage != t.first_stage and stage not in t.stages:
      continue
    if t.uses < min_uses:
      continue
    out.append(t)
  return out


def sort_textures(textures, by, order=()):
  by_uses = lambda t: (-t.uses, -t.frames, t.key)
  if by == 'uses':
    return sorted(textures, key=by_uses)
  if by == 'frames':
    return sorted(textures, key=lambda t: (-t.frames, -t.uses, t.key))
  if by == 'size':
    return sorted(textures,
                  key=lambda t: (-t.width * t.height, -t.uses, t.key))
  if by == 'stage':
    rank = _stage_rank(order)
    return sorted(textures, key=lambda t: (rank(t.first_stage),) + by_uses(t))
  raise PackError(f'unknown sort: {by}')


def _row(t):
  stages = len(t.stages) if t.in_csv else '?'
  uses = t.uses if t.in_csv else '?'
  frames = t.frames if t.in_csv else '?'
  return (f'{t.key}  {t.width:>4}x{t.height:<4} {t.format or "?":<6} '
          f'{t.memory or "?":<8} {t.first_stage or "-":<12} {stages:>6} '
          f'{uses:>10} {frames:>8}{"" if t.png else "  (no PNG)"}')


def report(dump_dir, sort='uses', stage=None, limit=None, keys_only=False,
           out=print):
  textures, has_csv = load_dump(dump_dir)
  chosen = sort_textures(select(textures, stage), sort, story_order())
  if limit is not None:
    chosen = chosen[:limit]
  if keys_only:
    for t in chosen:
      out(t.key)
    return chosen
  if not has_csv:
    out(f'note: {dump_dir} has no {DUMP_FILE}: sizes come from the PNGs, '
        'with no uses or stages (dump again with this version of mpo_boot)')
  out(f'{"key":<24}  {"size":<9} {"format":<6} {"memory":<8} '
      f'{"first stage":<12} {"stages":>6} {"uses":>10} {"frames":>8}')
  last_stage = object()
  for t in chosen:
    if sort == 'stage' and t.first_stage != last_stage:
      group = [x for x in chosen if x.first_stage == t.first_stage]
      label = t.first_stage or '(before any stage)'
      out(f'# {label}: {len(group)} textures, '
          f'{sum(x.uses for x in group)} uses')
      last_stage = t.first_stage
    out(_row(t))
  total_pixels = sum(t.width * t.height for t in chosen)
  out(f'{len(chosen)} of {len(textures)} textures, '
      f'{sum(t.uses for t in chosen)} uses, '
      f'{sum(1 for t in chosen if "vram" in t.memory)} in VRAM, '
      f'{total_pixels} texels')
  return chosen


# ---- textures.ini ----


def _parse_ini_line(line):
  """IniFile's ParseLine: (key, value) of "key = value # comment", or None."""
  key = ''
  pos = 0
  while True:
    next_pos = min((i for i in (line.find('=', pos), line.find('#', pos))
                    if i >= 0), default=-1)
    if next_pos <= 0:
      return None
    if line[next_pos] == '#':
      if line[next_pos - 1] != '\\':
        return None
      key += line[pos:next_pos - 1] + '#'
      pos = next_pos + 1
      continue
    key += line[pos:next_pos]
    pos = next_pos + 1
    break
  rest = line[pos:].strip()
  if len(rest) >= 2 and rest[0] == '"' and rest[-1] == '"':
    return key.strip(), rest[1:-1]
  value = ''
  while pos < len(line):
    hash_pos = line.find('#', pos)
    if hash_pos < 0:
      value += line[pos:]
      break
    if line[hash_pos - 1] != '\\':
      value += line[pos:hash_pos]
      break
    value += line[pos:hash_pos - 1] + '#'
    pos = hash_pos + 1
  return key.strip(), value.strip()


def parse_hash_name(text):
  """ParseHashName: (24-digit key, level) like sscanf("%16llx%8x_%d").

  None when the text does not start with a hex digit.
  """
  match = re.match(r'[ \t]*([0-9A-Fa-f]{1,16})([0-9A-Fa-f]{0,8})'
                   r'(?:_([0-9]{1,3}))?', text)
  if not match:
    return None
  cachekey = int(match.group(1), 16)
  hash_part = int(match.group(2), 16) if match.group(2) else 0
  level = int(match.group(3)) if match.group(3) else 0
  return f'{cachekey:016x}{hash_part:08x}', level


@dataclasses.dataclass
class Ini:
  hash: str = ''
  ignore_address: bool = False
  # (line, key as written, (24-digit key, level), file)
  hashes: list = dataclasses.field(default_factory=list)
  warnings: list = dataclasses.field(default_factory=list)


def _bool(value):
  lower = value.lower()
  if lower in ('1', 'true'):
    return True
  if lower in ('0', 'false'):
    return False
  return None


def parse_textures_ini(text):
  """ParseTexturesIni (host/texreplace/textures_ini.cpp). Raises PackError."""
  if text.startswith('﻿'):
    text = text[1:]
  ini = Ini()
  section = ''
  for number, line in enumerate(text.split('\n'), 1):
    line = line.rstrip('\r')
    stripped = line.strip()
    if not stripped or stripped[0] in '#;':
      continue
    if stripped[0] == '[':
      close = stripped.find(']')
      if close < 0:
        ini.warnings.append(f'line {number}: section without "]", ignored')
        section = '?'
        continue
      section = stripped[1:close].strip().lower()
      if section not in ('options', 'hashes'):
        ini.warnings.append(f'[{section}] is not used by this version of the '
                            'port')
      continue
    parsed = _parse_ini_line(line)
    if parsed is None:
      continue
    key, value = parsed
    if section == 'options':
      lower = key.lower()
      if lower == 'hash':
        if value.lower() not in ('xxh64', 'xxh32'):
          raise PackError(f'hash = {value} is not supported, use xxh64')
        ini.hash = value.lower()
      elif lower == 'ignoreaddress':
        flag = _bool(value)
        if flag is None:
          ini.warnings.append(f'ignoreAddress = {value}: not a boolean, false')
        ini.ignore_address = bool(flag)
      elif lower == 'reducehash' and _bool(value):
        raise PackError('reduceHash = true is not supported')
    elif section == 'hashes':
      name = parse_hash_name(key)
      if name is None:
        ini.warnings.append(f'line {number}: unsupported syntax under '
                            f'[hashes], ignored: {key}')
        continue
      ini.hashes.append((number, key, name, value.split('|')[0]))
  if not ini.hash:
    raise PackError('hash type not specified ([options] hash = xxh64)')
  return ini


def _load_ini(folder):
  path = os.path.join(folder, 'textures.ini')
  if not os.path.isfile(path):
    raise PackError(f'{folder}: no textures.ini')
  raw = _read_file(path, 16 << 20)
  try:
    return parse_textures_ini(raw.decode('utf-8'))
  except UnicodeDecodeError:
    raise PackError(f'{path}: not UTF-8') from None
  except PackError as error:
    raise PackError(f'{path}: {error}') from error


# ---- upscale ----


def nearest(width, height, pixels, channels, scale):
  """Repeats each pixel scale x scale times."""
  out = bytearray()
  for y in range(height):
    row = pixels[y * width * channels:(y + 1) * width * channels]
    wide = b''.join(row[x * channels:(x + 1) * channels] * scale
                    for x in range(width))
    out += wide * scale
  return bytes(out)


def _alpha_info(rgba):
  alpha = rgba[3::4]
  values = set(alpha)
  return alpha, values <= {255}, values <= {0, 255}


def realesrgan_argv(exe, in_dir, out_dir, scale, model, models_dir=None):
  """The command line of realesrgan-ncnn-vulkan on a folder of PNGs."""
  argv = list(exe) + ['-i', str(in_dir), '-o', str(out_dir), '-s', str(scale),
                      '-n', model, '-f', 'png']
  if models_dir:
    argv += ['-m', str(models_dir)]
  return argv


def command_argv(template, input_path, output_path, scale):
  if not any('{input}' in arg for arg in template):
    raise PackError('the command has no {input}')
  if not any('{output}' in arg for arg in template):
    raise PackError('the command has no {output}')
  return [arg.replace('{input}', str(input_path))
          .replace('{output}', str(output_path))
          .replace('{scale}', str(scale)) for arg in template]


def _run(argv, log):
  try:
    result = subprocess.run(argv, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False)
  except OSError as error:
    raise PackError(f'cannot run {argv[0]}: {error}') from error
  if result.returncode != 0:
    tail = result.stdout.decode('utf-8', 'replace').strip().splitlines()[-5:]
    for line in tail:
      log(f'  {line}')
    raise PackError(f'{os.path.basename(argv[0])} exited with '
                    f'{result.returncode}')


@dataclasses.dataclass
class UpscaleResult:
  written: list = dataclasses.field(default_factory=list)
  skipped: list = dataclasses.field(default_factory=list)
  failed: list = dataclasses.field(default_factory=list)


def upscale(dump_dir, pack_dir, scale=4, keys=None, stage=None, min_uses=0,
            top=None, realesrgan=None, model='realesrgan-x4plus',
            models_dir=None, command=None, alpha='scaler', overwrite=False,
            log=print):
  """Scales the dump's PNGs into pack_dir. Returns an UpscaleResult.

  realesrgan: the argv prefix of realesrgan-ncnn-vulkan (run once on a
  folder); command: a template run once per image ({input}, {output},
  {scale}). One of the two.
  """
  if not 1 <= scale <= 8:
    raise PackError('--scale goes from 1 to 8')
  if (realesrgan is None) == (command is None):
    raise PackError('give either realesrgan or a command')
  if command is not None:
    command_argv(command, 'in.png', 'out.png', scale)  # Checks the template.
  textures, _ = load_dump(dump_dir)
  if keys is not None:
    for key in keys:
      if not _KEY.match(key):
        raise PackError(f'bad key: {key}')
    chosen = [textures[k] for k in dict.fromkeys(keys) if k in textures]
    for key in dict.fromkeys(keys):
      if key not in textures:
        log(f'warning: {key} is not in the dump')
      elif not textures[key].png:
        log(f'warning: {key} has no PNG in the dump')
  else:
    chosen = select(textures, stage, min_uses)
  chosen = sort_textures([t for t in chosen if t.png], 'uses')
  if top is not None:
    chosen = chosen[:top]
  os.makedirs(pack_dir, exist_ok=True)
  if os.path.samefile(dump_dir, pack_dir):
    raise PackError('the pack folder must not be the dump folder')
  result = UpscaleResult()
  # The pack's textures.ini: the dump's (its keys were computed with it).
  ini_path = os.path.join(pack_dir, 'textures.ini')
  if not os.path.exists(ini_path):
    source = os.path.join(dump_dir, 'textures.ini')
    if os.path.isfile(source):
      _load_ini(dump_dir)  # It must be one the runtime takes.
      shutil.copyfile(source, ini_path)
    else:
      with open(ini_path, 'w', encoding='utf-8', newline='\n') as out:
        out.write(DEFAULT_TEXTURES_INI)
  work = []
  # The work folder goes inside the pack (the runtime only reads the pack's
  # root), so the game's art never lands in the system's temporary folder.
  with tempfile.TemporaryDirectory(prefix='.work-', dir=pack_dir) as tmp:
    in_dir = os.path.join(tmp, 'in')
    out_dir = os.path.join(tmp, 'out')
    os.makedirs(in_dir)
    os.makedirs(out_dir)
    for t in chosen:
      target = os.path.join(pack_dir, f'{t.key}.png')
      if os.path.exists(target) and not overwrite:
        result.skipped.append(t.key)
        continue
      if max(t.width, t.height) * scale > MAX_PACK_SIDE:
        log(f'warning: {t.key}: {t.width}x{t.height} x{scale} is larger than '
            f'{MAX_PACK_SIDE}, skipped')
        result.skipped.append(t.key)
        continue
      try:
        width, height, rgba = load_png(os.path.join(dump_dir, f'{t.key}.png'),
                                       MAX_PACK_SIDE)
      except PackError as error:
        log(f'error: {error}')
        result.failed.append(t.key)
        continue
      if t.in_csv and (width, height) != (t.width, t.height):
        log(f'error: {t.key}.png is {width}x{height}, dump.csv says '
            f'{t.width}x{t.height}')
        result.failed.append(t.key)
        continue
      alpha_plane, opaque, binary = _alpha_info(rgba)
      rgb = bytearray(width * height * 3)
      for c in range(3):
        rgb[c::3] = rgba[c::4]
      save_png(os.path.join(in_dir, f'{t.key}.png'), width, height, rgb, 3)
      if not opaque and alpha == 'scaler':
        grey = bytearray(width * height * 3)
        for c in range(3):
          grey[c::3] = alpha_plane
        save_png(os.path.join(in_dir, f'{t.key}_alpha.png'), width, height,
                 grey, 3)
      work.append((t.key, width, height, alpha_plane, opaque, binary))
    if not work:
      return result
    names = sorted(os.listdir(in_dir))
    log(f'scaling {len(work)} textures ({len(names)} images) x{scale}')
    if realesrgan is not None:
      _run(realesrgan_argv(realesrgan, in_dir, out_dir, scale, model,
                           models_dir), log)
    else:
      for name in names:
        try:
          _run(command_argv(command, os.path.join(in_dir, name),
                            os.path.join(out_dir, name), scale), log)
        except PackError as error:
          log(f'error: {name}: {error}')
    for key, width, height, alpha_plane, opaque, binary in work:
      try:
        rgba = _combine(out_dir, key, width, height, scale, alpha_plane,
                        opaque, binary, alpha)
      except PackError as error:
        log(f'error: {key}: {error}')
        result.failed.append(key)
        continue
      save_png(os.path.join(pack_dir, f'{key}.png'), width * scale,
               height * scale, rgba, 4)
      result.written.append(key)
  return result


def _scaled(out_dir, name, width, height):
  path = os.path.join(out_dir, name)
  if not os.path.isfile(path):
    raise PackError(f'the scaler wrote no {name}')
  w, h, pixels = load_png(path, MAX_PACK_SIDE)
  if (w, h) != (width, height):
    raise PackError(f'the scaler wrote {w}x{h}, not {width}x{height} (does '
                    'the model support this --scale?)')
  return pixels


def _combine(out_dir, key, width, height, scale, alpha_plane, opaque, binary,
             alpha_mode):
  w, h = width * scale, height * scale
  rgba = bytearray(_scaled(out_dir, f'{key}.png', w, h))
  if opaque:
    rgba[3::4] = b'\xff' * (w * h)
  elif alpha_mode == 'nearest':
    rgba[3::4] = nearest(width, height, alpha_plane, 1, scale)
  else:
    grey = _scaled(out_dir, f'{key}_alpha.png', w, h)
    r, g, b = grey[0::4], grey[1::4], grey[2::4]
    if binary:
      plane = bytes(255 if x + y + z >= 384 else 0 for x, y, z in zip(r, g, b))
    else:
      plane = bytes((x + y + z + 1) // 3 for x, y, z in zip(r, g, b))
    rgba[3::4] = plane
  return bytes(rgba)


# ---- check ----


@dataclasses.dataclass
class CheckResult:
  errors: list = dataclasses.field(default_factory=list)
  warnings: list = dataclasses.field(default_factory=list)
  checked: int = 0


def _check_image(result, label, path, original):
  try:
    width, height = check_png_data(_read_file(path))
  except (PackError, OSError) as error:
    result.errors.append(f'{label}: {error}')
    return
  result.checked += 1
  if original is None:
    return
  ow, oh = original
  if width > MAX_PACK_SIDE or height > MAX_PACK_SIDE:
    result.errors.append(f'{label}: {width}x{height} is larger than '
                         f'{MAX_PACK_SIDE}x{MAX_PACK_SIDE}')
  elif width % ow or height % oh or width // ow != height // oh:
    result.errors.append(
        f'{label}: {width}x{height} is not a whole multiple of the original '
        f'{ow}x{oh} with the same aspect (x2: {ow * 2}x{oh * 2}, x4: '
        f'{ow * 4}x{oh * 4})')


def _safe_relative(file):
  normalized = file.replace('\\', '/')
  if not normalized or normalized.startswith('/') or re.match(
      r'^[A-Za-z]:', normalized):
    return None
  parts = normalized.split('/')
  if any(p in ('', '.', '..') for p in parts):
    return None
  return parts


def check(pack_dir, dump_dir):
  """Validates a pack against the dump it was made from."""
  result = CheckResult()
  textures, _ = load_dump(dump_dir)
  if not os.path.isdir(pack_dir):
    raise PackError(f'{pack_dir}: not a folder')
  try:
    ini = _load_ini(pack_dir)
  except PackError as error:
    result.errors.append(str(error))
    ini = None
  if ini is not None:
    result.warnings.extend(f'textures.ini: {w}' for w in ini.warnings)
    try:
      dump_ini = _load_ini(dump_dir)
    except PackError:
      dump_ini = None
    if dump_ini is not None and (
        (ini.hash, ini.ignore_address) !=
        (dump_ini.hash, dump_ini.ignore_address)):
      result.errors.append(
          f'textures.ini: hash = {ini.hash}, ignoreAddress = '
          f'{str(ini.ignore_address).lower()}, but the dump was made with '
          f'hash = {dump_ini.hash}, ignoreAddress = '
          f'{str(dump_ini.ignore_address).lower()}: the keys would not match')

  def original(key):
    t = textures.get(key)
    return None if t is None else (t.width, t.height)

  listed = {}
  if ini is not None:
    for number, raw_key, (key, level), file in ini.hashes:
      label = f'textures.ini line {number}'
      if level != 0:
        result.warnings.append(f'{label}: mip level {level} is not checked')
        continue
      if not _KEY.match(raw_key.strip()):
        result.warnings.append(f'{label}: "{raw_key}" reads as {key}; write '
                               'the 24 lower-case digits of the dump')
      if key in listed:
        result.warnings.append(f'{label}: {key} is listed twice; the last '
                               'one is used')
      listed[key] = file
      if not file:
        continue  # Listed as "do not replace".
      if key not in textures:
        result.errors.append(f'{label}: {key} is not in the dump')
      parts = _safe_relative(file)
      if parts is None:
        result.errors.append(f'{label}: {file}: the file must be a relative '
                             'path inside the pack')
        continue
      path = os.path.join(pack_dir, *parts)
      if not os.path.isfile(path):
        result.errors.append(f'{label}: {file} does not exist')
        continue
      if not file.lower().endswith('.png'):
        result.errors.append(f'{label}: {file} is not a .png')
        continue
      _check_image(result, file, path, original(key))
  for name in sorted(os.listdir(pack_dir)):
    path = os.path.join(pack_dir, name)
    if not os.path.isfile(path):
      continue
    stem, ext = os.path.splitext(name)
    if name == DUMP_FILE:
      result.warnings.append(f'{name}: a pack does not need the dump\'s '
                             'data file')
      continue
    if ext.lower() != '.png':
      continue
    match = _HASH_NAME.match(stem)
    referenced = any(_safe_relative(f) == [name] for f in listed.values())
    if not match:
      if not referenced:
        result.warnings.append(f'{name}: not named by a key and not listed '
                               'under [hashes]: the port ignores it')
      continue
    if match.group(2) is not None:
      result.warnings.append(f'{name}: mip level {int(match.group(2))} is '
                             'not checked')
      continue
    key = match.group(1).lower()
    if ext != '.png' or match.group(1) != key:
      result.warnings.append(f'{name}: write it as {key}.png, like the dump')
    if key in listed:
      if listed[key] != name:
        result.warnings.append(f'{name}: [hashes] gives {key} another file '
                               f'({listed[key] or "do not replace"}), which '
                               'wins')
      continue
    if key not in textures:
      result.errors.append(f'{name}: {key} is not in the dump')
      continue
    _check_image(result, name, path, original(key))
  return result


# ---- command line ----


def _find_realesrgan(given):
  exe = given or 'realesrgan-ncnn-vulkan'
  found = shutil.which(exe)
  if found is None:
    raise PackError(
        f'{exe} not found. Install realesrgan-ncnn-vulkan '
        '(https://github.com/xinntao/Real-ESRGAN/releases) and pass its path '
        'with --realesrgan, or give another scaler after "--"')
  return [found]


def main(argv=None):
  argv = list(sys.argv[1:] if argv is None else argv)
  command = None
  if '--' in argv:
    at = argv.index('--')
    argv, command = argv[:at], argv[at + 1:]
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  rep = commands.add_parser('report', help='the dumped textures by use')
  rep.add_argument('dump')
  rep.add_argument('--sort', choices=('uses', 'frames', 'size', 'stage'),
                   default='uses')
  rep.add_argument('--stage', help='only the textures drawn in this stage')
  rep.add_argument('--limit', type=int)
  rep.add_argument('--keys-only', action='store_true',
                   help='one key per line (for upscale --keys)')
  up = commands.add_parser('upscale', help='scale the dump into a pack')
  up.add_argument('dump')
  up.add_argument('pack')
  up.add_argument('--scale', type=int, default=4)
  up.add_argument('--realesrgan', help='path of realesrgan-ncnn-vulkan')
  up.add_argument('--model', default='realesrgan-x4plus')
  up.add_argument('--models', help='folder of the models (its -m)')
  up.add_argument('--alpha', choices=('scaler', 'nearest'), default='scaler')
  up.add_argument('--stage')
  up.add_argument('--min-uses', type=int, default=0)
  up.add_argument('--top', type=int)
  up.add_argument('--keys', nargs='+')
  up.add_argument('--overwrite', action='store_true')
  chk = commands.add_parser('check', help='validate a pack against its dump')
  chk.add_argument('pack')
  chk.add_argument('dump')
  args = parser.parse_args(argv)
  for name in ('limit', 'top'):
    if getattr(args, name, None) is not None and getattr(args, name) < 0:
      parser.error(f'--{name} must not be negative')
  if command is not None and args.command != 'upscale':
    parser.error('"--" (a scaler command) only goes with upscale')
  try:
    if args.command == 'report':
      report(args.dump, args.sort, args.stage, args.limit, args.keys_only)
      return 0
    if args.command == 'upscale':
      if command is not None and not command:
        parser.error('no command after "--"')
      realesrgan = None if command else _find_realesrgan(args.realesrgan)
      result = upscale(args.dump, args.pack, args.scale, args.keys, args.stage,
                       args.min_uses, args.top, realesrgan, args.model,
                       args.models, command, args.alpha, args.overwrite)
      print(f'{len(result.written)} written, {len(result.skipped)} skipped '
            f'(already in the pack or too large), {len(result.failed)} '
            f'failed: {args.pack}')
      return 1 if result.failed else 0
    result = check(args.pack, args.dump)
    for line in result.errors:
      print(f'error: {line}')
    for line in result.warnings:
      print(f'warning: {line}')
    print(f'{result.checked} PNGs checked, {len(result.errors)} errors, '
          f'{len(result.warnings)} warnings')
    return 1 if result.errors else 0
  except (PackError, OSError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 1


if __name__ == '__main__':
  sys.exit(main())
