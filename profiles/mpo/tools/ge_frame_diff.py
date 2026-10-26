#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-pixel comparison of two framebuffer PNGs.

Compares a render of `mpo_ppdmp_replay` (F11a) with a reference exported from
PPSSPP's GE debugger (software renderer, same .ppdmp). Only the standard
library is used; run it with `python -I`.

  python3 -I profiles/mpo/tools/ge_frame_diff.py ours.png reference.png \
      [--width 480] [--height 272] [--alpha] [--tolerance N] [--diff-out d.png]

Both images are compared from their top-left corner over `width` x `height`
pixels (PPSSPP may export the full stride, e.g. 512 wide). RGB is compared by
default; `--alpha` also compares alpha (the stencil bit of a 5551 buffer).
A pixel differs when any compared channel differs by more than `tolerance`.
`--diff-out` writes a PNG with equal pixels darkened and differing pixels in
red (brighter is larger). The images hold game pixels: keep them and the diff
in an ignored folder such as analysis/f11a/.

Exit status: 0 when no pixel differs, 1 when some do, 2 on errors.
"""

import argparse
import struct
import sys
import zlib

_SIGNATURE = b'\x89PNG\r\n\x1a\n'
_MAX_SIDE = 8192
_CHANNELS = {2: 3, 6: 4}


class PngError(Exception):
  """Raised for PNG files this tool cannot read."""


def _paeth(a, b, c):
  p = a + b - c
  pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
  if pa <= pb and pa <= pc:
    return a
  return b if pb <= pc else c


def _unfilter(raw, width, height, bpp):
  stride = width * bpp
  if len(raw) != (stride + 1) * height:
    raise PngError(f'image data holds {len(raw)} bytes, expected '
                   f'{(stride + 1) * height}')
  out = bytearray(stride * height)
  prev = bytearray(stride)
  pos = 0
  for y in range(height):
    kind = raw[pos]
    line = bytearray(raw[pos + 1:pos + 1 + stride])
    pos += stride + 1
    if kind == 1:
      for i in range(bpp, stride):
        line[i] = (line[i] + line[i - bpp]) & 0xFF
    elif kind == 2:
      for i in range(stride):
        line[i] = (line[i] + prev[i]) & 0xFF
    elif kind == 3:
      for i in range(stride):
        left = line[i - bpp] if i >= bpp else 0
        line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
    elif kind == 4:
      for i in range(stride):
        left = line[i - bpp] if i >= bpp else 0
        upleft = prev[i - bpp] if i >= bpp else 0
        line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
    elif kind != 0:
      raise PngError(f'unknown filter type {kind} on row {y}')
    out[y * stride:(y + 1) * stride] = line
    prev = line
  return bytes(out)


def read_png(data):
  """Decodes an 8-bit RGB or RGBA PNG. Returns (width, height, rgba bytes)."""
  if not data.startswith(_SIGNATURE):
    raise PngError('not a PNG file')
  pos = len(_SIGNATURE)
  header = None
  idat = bytearray()
  while True:
    if pos + 8 > len(data):
      raise PngError('chunk header is truncated')
    length, kind = struct.unpack_from('>I4s', data, pos)
    if pos + 12 + length > len(data):
      raise PngError(f'chunk {kind!r} is truncated')
    body = data[pos + 8:pos + 8 + length]
    (crc,) = struct.unpack_from('>I', data, pos + 8 + length)
    if zlib.crc32(kind + body) != crc:
      raise PngError(f'chunk {kind!r} has a bad CRC')
    pos += 12 + length
    if kind == b'IHDR':
      if length != 13:
        raise PngError('bad IHDR')
      header = struct.unpack('>IIBBBBB', body)
    elif kind == b'IDAT':
      idat += body
    elif kind == b'IEND':
      break
  if header is None:
    raise PngError('no IHDR chunk')
  width, height, depth, color, _, _, interlace = header
  if not (0 < width <= _MAX_SIDE and 0 < height <= _MAX_SIDE):
    raise PngError(f'unsupported size {width}x{height}')
  if depth != 8 or color not in _CHANNELS or interlace != 0:
    raise PngError(f'unsupported format (depth {depth}, colour type {color}, '
                   f'interlace {interlace}); expected 8-bit RGB or RGBA')
  bpp = _CHANNELS[color]
  expected = (width * bpp + 1) * height
  inflater = zlib.decompressobj()
  try:
    raw = inflater.decompress(bytes(idat), expected + 1)
  except zlib.error as error:
    raise PngError(f'corrupt image data: {error}') from error
  pixels = _unfilter(raw, width, height, bpp)
  if bpp == 4:
    return width, height, pixels
  rgba = bytearray(width * height * 4)
  rgba[0::4] = pixels[0::3]
  rgba[1::4] = pixels[1::3]
  rgba[2::4] = pixels[2::3]
  rgba[3::4] = b'\xff' * (width * height)
  return width, height, bytes(rgba)


def write_png(path, width, height, rgba):
  """Writes 8-bit RGBA pixels as a PNG."""
  rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4]
                  for y in range(height))

  def chunk(kind, body):
    return (struct.pack('>I', len(body)) + kind + body +
            struct.pack('>I', zlib.crc32(kind + body)))

  with open(path, 'wb') as out:
    out.write(_SIGNATURE)
    out.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0,
                                         0, 0)))
    out.write(chunk(b'IDAT', zlib.compress(rows, 9)))
    out.write(chunk(b'IEND', b''))


def compare(ours, reference, width, height, alpha=False, tolerance=0):
  """Compares two decoded images over width x height from the top-left.

  Returns a dict with the counts and, under 'diff', RGBA bytes of a
  visualisation (equal pixels darkened, differing ones in red).
  """
  ow, oh, opix = ours
  rw, rh, rpix = reference
  if ow < width or oh < height or rw < width or rh < height:
    raise PngError(f'images are {ow}x{oh} and {rw}x{rh}; both must be at '
                   f'least {width}x{height}')
  channels = 4 if alpha else 3
  differing = 0
  max_delta = [0, 0, 0, 0]
  buckets = {'1': 0, '2-3': 0, '4-7': 0, '8-15': 0, '16+': 0}
  box = None
  diff = bytearray(width * height * 4)
  for y in range(height):
    for x in range(width):
      oi = (y * ow + x) * 4
      ri = (y * rw + x) * 4
      worst = 0
      for c in range(channels):
        delta = abs(opix[oi + c] - rpix[ri + c])
        max_delta[c] = max(max_delta[c], delta)
        worst = max(worst, delta)
      di = (y * width + x) * 4
      if worst > tolerance:
        differing += 1
        if worst == 1:
          buckets['1'] += 1
        elif worst < 4:
          buckets['2-3'] += 1
        elif worst < 8:
          buckets['4-7'] += 1
        elif worst < 16:
          buckets['8-15'] += 1
        else:
          buckets['16+'] += 1
        box = ((x, y, x, y) if box is None else
               (min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)))
        diff[di:di + 4] = bytes((min(255, 96 + worst * 8), 0, 0, 255))
      else:
        gray = (opix[oi] + opix[oi + 1] + opix[oi + 2]) // 12
        diff[di:di + 4] = bytes((gray, gray, gray, 255))
  return {
      'pixels': width * height,
      'differing': differing,
      'max_delta': max_delta[:channels],
      'buckets': buckets,
      'box': box,
      'diff': bytes(diff),
  }


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('ours')
  parser.add_argument('reference')
  parser.add_argument('--width', type=int, default=480)
  parser.add_argument('--height', type=int, default=272)
  parser.add_argument('--alpha', action='store_true',
                      help='also compare alpha (stencil)')
  parser.add_argument('--tolerance', type=int, default=0,
                      help='per-channel difference still counted as equal')
  parser.add_argument('--diff-out', help='write a visualisation PNG here')
  args = parser.parse_args(argv)
  try:
    with open(args.ours, 'rb') as f:
      ours = read_png(f.read())
    with open(args.reference, 'rb') as f:
      reference = read_png(f.read())
    result = compare(ours, reference, args.width, args.height, args.alpha,
                     args.tolerance)
    if args.diff_out:
      write_png(args.diff_out, args.width, args.height, result['diff'])
  except (OSError, PngError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 2
  share = 100.0 * result['differing'] / result['pixels']
  print(f'compared {result["pixels"]} pixels '
        f'({args.width}x{args.height}, {"RGBA" if args.alpha else "RGB"}, '
        f'tolerance {args.tolerance})')
  print(f'differing: {result["differing"]} ({share:.3f} %)')
  if result['differing']:
    print('max delta per channel: ' +
          ' '.join(str(d) for d in result['max_delta']))
    print('by largest channel delta: ' +
          ', '.join(f'{k}: {v}' for k, v in result['buckets'].items()))
    x0, y0, x1, y1 = result['box']
    print(f'bounding box of differences: ({x0},{y0})-({x1},{y1})')
  return 1 if result['differing'] else 0


if __name__ == '__main__':
  sys.exit(main())
