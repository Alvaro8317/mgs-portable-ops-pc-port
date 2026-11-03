#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rasterizes the fallback font of the PPGe dialogs into fallback_font_glyphs.inc.

Without the firmware fonts (flash0:/font/*.pgf, which the player provides),
PPGe draws the sceUtility dialogs with GNU FreeFont's FreeSans (F23c). The
glyphs are rasterized here, once, and the table is committed: the builds (and
the CI) only compile the generated C++ file, and the runtime needs no font file
and no rasterizer. The font and its license are described in
third_party/freefont/NOTICE.md.

The rasterizer is self-contained (no FreeType, no Pillow), so the output only
depends on this script and on the font file: it reads the TrueType outlines,
flattens the quadratic curves and computes the exact area each pixel covers
(the signed-area accumulation of font-rs), without hinting.

Usage:
  python3 -I make_fallback_font.py --zip freefont-ttf-20120503.zip
  python3 -I make_fallback_font.py --ttf FreeSans.ttf
  python3 -I make_fallback_font.py --download        # from ftp.gnu.org
  python3 -I make_fallback_font.py --check [--zip ...|--ttf ...]

--check fails if the .inc was made by another version of this script. When the
font is given (or MPO_FREEFONT_ZIP / MPO_FREEFONT_TTF name it), it also
regenerates the table and compares it byte by byte; otherwise it says that it
skipped that part.
"""

import argparse
import hashlib
import io
import math
import os
import pathlib
import struct
import sys
import tempfile
import urllib.request
import zipfile

FONT_URL = 'https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip'
ZIP_SHA256 = '7c85baf1bf82a1a1845d1322112bc6ca982221b484e3b3925022e25b5cae89af'
TTF_MEMBER = 'freefont-20120503/FreeSans.ttf'
TTF_SHA256 = 'c80858440d8fb618e0ac5ff6f16251dbfa6b3316f00f3cdd17d477297dd87b04'
OUTPUT = 'fallback_font_glyphs.inc'

# The em of the firmware's ltn0.pgf: 10.125 points at 128 dpi.
EM_PIXELS = 18
# Maximum distance, in pixels, between a curve and its flattened segments.
FLATNESS = 0.05
MAX_FONT_BYTES = 16 << 20

# What the dialogs draw: ASCII, Latin-1 (the button symbol × is in it), the
# typographic punctuation of English, French, German, Italian and Spanish
# texts, and the other three button symbols (○ △ □).
CHARACTERS = (
    list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) +
    [0x0152, 0x0153, 0x0178, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
     0x2022, 0x2026, 0x20AC, 0x2122, 0x25A1, 0x25B3, 0x25CB, 0xFFFD])
# The line metrics come from these: the ascender and descender of the
# characters a line usually holds, so that accents above capitals do not make
# every line taller (as ltn0's own maxAscender, 13.5 px, does not cover them).
METRIC_CHARACTERS = list(range(0x20, 0x7F)) + [0x25A1, 0x25B3, 0x25CB]


class FontError(Exception):
  """The font file is not what this script can read."""


class TrueTypeFont:
  """The parts of a TrueType (glyf) font the rasterizer needs."""

  def __init__(self, data):
    self.data = data
    if len(data) < 12:
      raise FontError('file too small')
    (version, num_tables) = struct.unpack_from('>IH', data, 0)
    if version not in (0x00010000, 0x74727565):
      raise FontError(f'not a TrueType font (version {version:08X})')
    self.tables = {}
    for i in range(num_tables):
      record = 12 + 16 * i
      if record + 16 > len(data):
        raise FontError('table directory out of bounds')
      tag, _, offset, length = struct.unpack_from('>4sIII', data, record)
      if offset + length > len(data):
        raise FontError(f'table {tag!r} out of bounds')
      self.tables[tag.decode('latin-1')] = (offset, length)
    for tag in ('head', 'maxp', 'hhea', 'hmtx', 'cmap', 'loca', 'glyf'):
      if tag not in self.tables:
        raise FontError(f'missing table {tag}')
    head = self._table('head', 54)
    self.units_per_em = struct.unpack_from('>H', data, head + 18)[0]
    self.long_loca = struct.unpack_from('>h', data, head + 50)[0] == 1
    self.num_glyphs = struct.unpack_from('>H', data, self._table('maxp', 6) + 4)[0]
    hhea = self._table('hhea', 36)
    self.num_hmetrics = struct.unpack_from('>H', data, hhea + 34)[0]
    if self.units_per_em == 0 or self.num_hmetrics == 0:
      raise FontError('invalid head or hhea')
    self.cmap = self._read_cmap()

  def _table(self, tag, min_size):
    offset, length = self.tables[tag]
    if length < min_size:
      raise FontError(f'table {tag} too small')
    return offset

  def _u16(self, offset):
    if offset + 2 > len(self.data):
      raise FontError('read out of bounds')
    return struct.unpack_from('>H', self.data, offset)[0]

  def _read_cmap(self):
    """Returns {code point: glyph} from the Windows Unicode BMP subtable."""
    base, length = self.tables['cmap']
    count = self._u16(base + 2)
    for i in range(count):
      platform, encoding, offset = struct.unpack_from('>HHI', self.data, base + 4 + 8 * i)
      if (platform, encoding) == (3, 1) and offset < length:
        sub = base + offset
        if self._u16(sub) == 4:
          return self._read_cmap4(sub)
    raise FontError('no format 4 Windows Unicode cmap')

  def _read_cmap4(self, sub):
    segments = self._u16(sub + 6) // 2
    ends = sub + 14
    starts = ends + 2 * segments + 2
    deltas = starts + 2 * segments
    ranges = deltas + 2 * segments
    mapping = {}
    for s in range(segments):
      end = self._u16(ends + 2 * s)
      start = self._u16(starts + 2 * s)
      delta = self._u16(deltas + 2 * s)
      range_offset = self._u16(ranges + 2 * s)
      if start > end:
        raise FontError('invalid cmap segment')
      for code in range(start, end + 1):
        if code == 0xFFFF:
          continue
        if range_offset == 0:
          glyph = (code + delta) & 0xFFFF
        else:
          glyph = self._u16(ranges + 2 * s + range_offset + 2 * (code - start))
          if glyph != 0:
            glyph = (glyph + delta) & 0xFFFF
        if glyph != 0:
          mapping[code] = glyph
    return mapping

  def advance(self, glyph):
    """The advance width of a glyph, in font units."""
    index = min(glyph, self.num_hmetrics - 1)
    offset, length = self.tables['hmtx']
    if 4 * index + 2 > length:
      raise FontError('hmtx out of bounds')
    return self._u16(offset + 4 * index)

  def _glyph_range(self, glyph):
    if glyph >= self.num_glyphs:
      raise FontError(f'glyph {glyph} out of range')
    loca, loca_length = self.tables['loca']
    if self.long_loca:
      if 4 * glyph + 8 > loca_length:
        raise FontError('loca out of bounds')
      start, end = struct.unpack_from('>II', self.data, loca + 4 * glyph)
    else:
      if 2 * glyph + 4 > loca_length:
        raise FontError('loca out of bounds')
      start, end = struct.unpack_from('>HH', self.data, loca + 2 * glyph)
      start, end = 2 * start, 2 * end
    glyf, glyf_length = self.tables['glyf']
    if start > end or end > glyf_length:
      raise FontError(f'glyph {glyph} out of the glyf table')
    return glyf + start, end - start

  def contours(self, glyph, depth=0):
    """The contours of a glyph: lists of (x, y, on_curve) in font units."""
    if depth > 8:
      raise FontError('composite glyphs nested too deep')
    offset, length = self._glyph_range(glyph)
    if length == 0:
      return []
    if length < 10:
      raise FontError(f'glyph {glyph} too small')
    num_contours = struct.unpack_from('>h', self.data, offset)[0]
    if num_contours < 0:
      return self._composite(offset + 10, offset + length, depth)
    return self._simple(offset + 10, offset + length, num_contours)

  def _simple(self, p, end, num_contours):
    d = self.data
    if p + 2 * num_contours + 2 > end:
      raise FontError('simple glyph out of bounds')
    ends = list(struct.unpack_from(f'>{num_contours}H', d, p))
    p += 2 * num_contours
    num_points = ends[-1] + 1 if ends else 0
    instructions = self._u16(p)
    p += 2 + instructions
    flags = []
    while len(flags) < num_points:
      if p >= end:
        raise FontError('glyph flags out of bounds')
      flag = d[p]
      p += 1
      repeat = 0
      if flag & 8:
        if p >= end:
          raise FontError('glyph flags out of bounds')
        repeat = d[p]
        p += 1
      flags.extend([flag] * (repeat + 1))
    flags = flags[:num_points]

    def read_coordinates(p, short_bit, same_bit):
      values = []
      value = 0
      for flag in flags:
        if flag & short_bit:
          if p >= end:
            raise FontError('glyph coordinates out of bounds')
          delta = d[p]
          p += 1
          value += delta if flag & same_bit else -delta
        elif not flag & same_bit:
          if p + 2 > end:
            raise FontError('glyph coordinates out of bounds')
          value += struct.unpack_from('>h', d, p)[0]
          p += 2
        values.append(value)
      return values, p

    xs, p = read_coordinates(p, 2, 16)
    ys, p = read_coordinates(p, 4, 32)
    contours = []
    start = 0
    for last in ends:
      if last < start or last >= num_points:
        raise FontError('invalid contour end')
      contours.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(start, last + 1)])
      start = last + 1
    return contours

  def _composite(self, p, end, depth):
    d = self.data
    contours = []
    while True:
      if p + 4 > end:
        raise FontError('composite glyph out of bounds')
      flags, component = struct.unpack_from('>HH', d, p)
      p += 4
      if flags & 1:
        if p + 4 > end:
          raise FontError('composite glyph out of bounds')
        dx, dy = struct.unpack_from('>hh', d, p)
        p += 4
      else:
        if p + 2 > end:
          raise FontError('composite glyph out of bounds')
        dx, dy = struct.unpack_from('>bb', d, p)
        p += 2
      if not flags & 2:
        raise FontError('composite glyphs placed by points are not supported')
      a, b, c, e = 1.0, 0.0, 0.0, 1.0

      def f2dot14(offset):
        return struct.unpack_from('>h', d, offset)[0] / 16384.0

      if flags & 8:
        a = e = f2dot14(p)
        p += 2
      elif flags & 0x40:
        a, e = f2dot14(p), f2dot14(p + 2)
        p += 4
      elif flags & 0x80:
        a, b, c, e = f2dot14(p), f2dot14(p + 2), f2dot14(p + 4), f2dot14(p + 6)
        p += 8
      if p > end:
        raise FontError('composite glyph out of bounds')
      for contour in self.contours(component, depth + 1):
        contours.append([(a * x + c * y + dx, b * x + e * y + dy, on)
                         for x, y, on in contour])
      if not flags & 0x20:
        return contours


def flatten(contour, scale):
  """Turns a TrueType contour into a closed polyline in pixels (y up)."""
  points = [(x * scale, y * scale, on) for x, y, on in contour]
  if not points:
    return []
  # Start on an on-curve point, inventing one between two off-curve points.
  first = next((i for i, p in enumerate(points) if p[2]), None)
  if first is None:
    a, b = points[-1], points[0]
    start = ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)
    order = points
  else:
    start = points[first][:2]
    order = points[first + 1:] + points[:first]
  order = order + [(start[0], start[1], True)]
  out = [start]
  current = start
  control = None
  for x, y, on in order:
    if on:
      if control is None:
        out.append((x, y))
      else:
        out.extend(quadratic(current, control, (x, y)))
        control = None
      current = (x, y)
    elif control is None:
      control = (x, y)
    else:
      middle = ((control[0] + x) / 2.0, (control[1] + y) / 2.0)
      out.extend(quadratic(current, control, middle))
      current = middle
      control = (x, y)
  return out


def quadratic(p0, p1, p2):
  """The points after p0 of a flattened quadratic Bézier, ending at p2."""
  ddx = p0[0] - 2.0 * p1[0] + p2[0]
  ddy = p0[1] - 2.0 * p1[1] + p2[1]
  # The error of n uniform segments is |dd| / (8 n²).
  error = math.sqrt(ddx * ddx + ddy * ddy) / 8.0
  n = max(1, min(64, math.ceil(math.sqrt(error / FLATNESS))))
  out = []
  for i in range(1, n + 1):
    t = i / n
    u = 1.0 - t
    out.append((u * u * p0[0] + 2.0 * u * t * p1[0] + t * t * p2[0],
                u * u * p0[1] + 2.0 * u * t * p1[1] + t * t * p2[1]))
  return out


def rasterize(polylines):
  """Returns (left, top, width, height, coverage bytes) of the polylines.

  left and top are the offset of the bitmap from the pen position, in pixels,
  with top measured upwards from the baseline (the convention of
  font::GlyphBitmap).
  """
  points = [p for line in polylines for p in line]
  if not points:
    return 0, 0, 0, 0, b''
  left = math.floor(min(p[0] for p in points))
  right = math.ceil(max(p[0] for p in points))
  bottom = math.floor(min(p[1] for p in points))
  top = math.ceil(max(p[1] for p in points))
  width = max(1, right - left)
  height = max(1, top - bottom)
  stride = width + 2
  area = [0.0] * (stride * height)
  for line in polylines:
    for i in range(len(line) - 1):
      x0, y0 = line[i][0] - left, top - line[i][1]
      x1, y1 = line[i + 1][0] - left, top - line[i + 1][1]
      accumulate_line(area, stride, height, x0, y0, x1, y1)
  coverage = bytearray(width * height)
  for y in range(height):
    total = 0.0
    for x in range(width):
      total += area[y * stride + x]
      value = min(1.0, abs(total))
      coverage[y * width + x] = int(value * 255.0 + 0.5)
  return left, top, width, height, bytes(coverage)


def accumulate_line(area, stride, height, x0, y0, x1, y1):
  """Adds the signed area of one edge to the accumulation buffer (font-rs)."""
  if y0 == y1:
    return
  direction = 1.0
  if y0 > y1:
    direction = -1.0
    x0, y0, x1, y1 = x1, y1, x0, y0
  dxdy = (x1 - x0) / (y1 - y0)
  x = x0
  if y0 < 0.0:
    x -= y0 * dxdy
    y0 = 0.0
  y1 = min(float(height), y1)
  if y0 >= y1:
    return
  for y in range(int(math.floor(y0)), int(math.ceil(y1))):
    row = y * stride
    dy = min(float(y + 1), y1) - max(float(y), y0)
    x_next = x + dxdy * dy
    d = dy * direction
    xa, xb = (x, x_next) if x < x_next else (x_next, x)
    xa = max(0.0, xa)
    xb = max(0.0, xb)
    xa_floor = math.floor(xa)
    xa_i = int(xa_floor)
    xb_ceil = math.ceil(xb)
    xb_i = int(xb_ceil)
    if xb_i <= xa_i + 1:
      middle = 0.5 * (xa + xb) - xa_floor
      area[row + xa_i] += d - d * middle
      area[row + xa_i + 1] += d * middle
    else:
      s = 1.0 / (xb - xa)
      xa_frac = xa - xa_floor
      a0 = 0.5 * s * (1.0 - xa_frac) * (1.0 - xa_frac)
      xb_frac = xb - xb_ceil + 1.0
      am = 0.5 * s * xb_frac * xb_frac
      area[row + xa_i] += d * a0
      if xb_i == xa_i + 2:
        area[row + xa_i + 1] += d * (1.0 - a0 - am)
      else:
        a1 = s * (1.5 - xa_frac)
        area[row + xa_i + 1] += d * (a1 - a0)
        for xi in range(xa_i + 2, xb_i - 1):
          area[row + xi] += d * s
        a2 = a1 + (xb_i - xa_i - 3) * s
        area[row + xb_i - 1] += d * (1.0 - a2 - am)
      area[row + xb_i] += d * am
    x = x_next


def to_26_6(value):
  """Pixels to 26.6 fixed point, rounding halves away from zero."""
  scaled = value * 64.0
  return int(math.floor(scaled + 0.5)) if scaled >= 0 else -int(math.floor(-scaled + 0.5))


def script_hash():
  """The SHA-256 of this script, with LF line endings."""
  return hashlib.sha256(pathlib.Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def generate(ttf_data):
  """Returns the text of the .inc for the FreeSans.ttf bytes."""
  font = TrueTypeFont(ttf_data)
  scale = EM_PIXELS / font.units_per_em
  glyphs = []
  missing = []
  for code in CHARACTERS:
    glyph = font.cmap.get(code)
    if glyph is None:
      missing.append(code)
      continue
    polylines = [flatten(c, scale) for c in font.contours(glyph)]
    left, top, width, height, pixels = rasterize([p for p in polylines if p])
    glyphs.append((code, to_26_6(font.advance(glyph) * scale), left, top, width, height, pixels))
  if missing:
    raise FontError('FreeSans lacks ' + ' '.join(f'U+{c:04X}' for c in missing))
  metric = [g for g in glyphs if g[0] in METRIC_CHARACTERS and g[4] > 0]
  ascender = max(g[3] for g in metric)
  descender = min(g[3] - g[5] for g in metric)

  out = io.StringIO()
  out.write('// SPDX-License-Identifier: GPL-3.0-or-later\n')
  out.write(f'// Generated by make_fallback_font.py from GNU FreeFont 20120503 FreeSans.ttf\n')
  out.write('// (third_party/freefont/NOTICE.md). Do not edit.\n')
  out.write(f'// generator-sha256 {script_hash()}\n')
  out.write(f'// font-sha256 {TTF_SHA256}\n')
  out.write('// clang-format off\n\n')
  out.write(f'inline constexpr int kFallbackEmPixels = {EM_PIXELS};\n')
  out.write(f'inline constexpr std::int32_t kFallbackAscender = {to_26_6(ascender)};  // 26.6\n')
  out.write(f'inline constexpr std::int32_t kFallbackDescender = {to_26_6(descender)};  // 26.6\n\n')
  out.write('// code, advance (26.6), left, top, width, height, offset into kFallbackPixels.\n')
  out.write(f'inline constexpr FallbackGlyph kFallbackGlyphs[{len(glyphs)}] = {{\n')
  offset = 0
  for code, advance, left, top, width, height, pixels in glyphs:
    out.write(f'    {{0x{code:04X}, {advance}, {left}, {top}, {width}, {height}, {offset}}},\n')
    offset += len(pixels)
  out.write('};\n\n')
  out.write(f'inline constexpr std::uint8_t kFallbackPixels[{max(1, offset)}] = {{\n')
  blob = b''.join(g[6] for g in glyphs) or b'\0'
  for i in range(0, len(blob), 16):
    out.write('    ' + ', '.join(f'0x{b:02X}' for b in blob[i:i + 16]) + ',\n')
  out.write('};\n')
  return out.getvalue()


def read_limited(path):
  data = pathlib.Path(path).read_bytes()
  if len(data) > MAX_FONT_BYTES * 2:
    raise FontError(f'{path}: too large')
  return data


def ttf_from_zip(zip_data):
  if hashlib.sha256(zip_data).hexdigest() != ZIP_SHA256:
    raise FontError('the FreeFont archive does not have the expected SHA-256')
  with zipfile.ZipFile(io.BytesIO(zip_data)) as archive:
    info = archive.getinfo(TTF_MEMBER)
    if info.file_size > MAX_FONT_BYTES:
      raise FontError(f'{TTF_MEMBER}: too large')
    return archive.read(info)


def load_font(args):
  """Returns the verified FreeSans.ttf bytes, or None when no font was given."""
  ttf = args.ttf or os.environ.get('MPO_FREEFONT_TTF')
  archive = args.zip or os.environ.get('MPO_FREEFONT_ZIP')
  if ttf:
    data = read_limited(ttf)
  elif archive:
    data = ttf_from_zip(read_limited(archive))
  elif args.download:
    with tempfile.TemporaryDirectory() as tmp:
      target = pathlib.Path(tmp) / 'freefont.zip'
      urllib.request.urlretrieve(FONT_URL, target)
      data = ttf_from_zip(read_limited(target))
  else:
    return None
  if hashlib.sha256(data).hexdigest() != TTF_SHA256:
    raise FontError('FreeSans.ttf does not have the expected SHA-256 (GNU FreeFont 20120503)')
  return data


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--ttf', help='FreeSans.ttf of GNU FreeFont 20120503')
  parser.add_argument('--zip', help='freefont-ttf-20120503.zip')
  parser.add_argument('--download', action='store_true', help=f'download {FONT_URL}')
  parser.add_argument('--check', action='store_true', help='fail if the .inc is stale')
  parser.add_argument('--output', default=str(pathlib.Path(__file__).with_name(OUTPUT)))
  args = parser.parse_args(argv)
  output = pathlib.Path(args.output)
  try:
    ttf = load_font(args)
    if args.check:
      current = output.read_text(encoding='utf-8').replace('\r\n', '\n')
      if f'// generator-sha256 {script_hash()}\n' not in current:
        print(f'{output.name} is stale: regenerate it with make_fallback_font.py --zip ...')
        return 1
      if ttf is None:
        print(f'{output.name}: generator hash matches; regeneration skipped (no font given)')
        return 0
      if generate(ttf) != current:
        print(f'{output.name} differs from a fresh generation')
        return 1
      print(f'{output.name}: up to date (regenerated and compared)')
      return 0
    if ttf is None:
      parser.error('give --ttf, --zip or --download')
    output.write_text(generate(ttf), encoding='utf-8', newline='\n')
    print(f'wrote {output}')
    return 0
  except (FontError, OSError, KeyError, struct.error, zipfile.BadZipFile) as error:
    print(f'make_fallback_font.py: {error}', file=sys.stderr)
    return 1


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
