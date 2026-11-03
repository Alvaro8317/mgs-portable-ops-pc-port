#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for profiles/mpo/host/font/make_fallback_font.py (F23c), without the font."""

import contextlib
import importlib.util
import io
import pathlib
import struct
import tempfile
import unittest

_ROOT = pathlib.Path(__file__).resolve().parents[3]


def _load(path):
  spec = importlib.util.spec_from_file_location(path.stem, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


make_fallback_font = _load(_ROOT / 'profiles' / 'mpo' / 'host' / 'font' / 'make_fallback_font.py')


def _square(x0, y0, x1, y1, clockwise=True):
  """A closed square polyline in pixels (y up)."""
  points = [(x0, y0), (x0, y1), (x1, y1), (x1, y0), (x0, y0)]
  return points if clockwise else list(reversed(points))


class RasterizeTest(unittest.TestCase):

  def test_square_on_half_pixels(self):
    left, top, width, height, pixels = make_fallback_font.rasterize([_square(0.5, 0.5, 2.5, 2.5)])
    self.assertEqual((left, top, width, height), (0, 3, 3, 3))
    # Corners a quarter covered, edges half, the centre whole.
    self.assertEqual(list(pixels), [64, 128, 64, 128, 255, 128, 64, 128, 64])

  def test_winding_does_not_matter_and_holes_are_empty(self):
    outer = _square(0.0, 0.0, 3.0, 3.0)
    _, _, _, _, solid = make_fallback_font.rasterize([outer])
    _, _, _, _, reversed_solid = make_fallback_font.rasterize([_square(0, 0, 3, 3, False)])
    self.assertEqual(solid, reversed_solid)
    self.assertEqual(set(solid), {255})
    _, _, _, _, ring = make_fallback_font.rasterize([outer, _square(1.0, 1.0, 2.0, 2.0, False)])
    self.assertEqual(ring[4], 0)
    self.assertEqual(ring[0], 255)

  def test_diagonal_edge_covers_half(self):
    triangle = [(0.0, 0.0), (0.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    self.assertEqual(make_fallback_font.rasterize([triangle])[4], bytes([128]))

  def test_empty_glyph(self):
    self.assertEqual(make_fallback_font.rasterize([]), (0, 0, 0, 0, b''))


class OutlineTest(unittest.TestCase):

  def test_off_curve_contour_is_a_closed_curve(self):
    # Four off-curve points: a circle-like curve through the midpoints.
    contour = [(0, 10, False), (10, 10, False), (10, 0, False), (0, 0, False)]
    line = make_fallback_font.flatten(contour, 1.0)
    self.assertEqual(line[0], line[-1])
    self.assertGreater(len(line), 8)
    for x, y in line:
      self.assertTrue(0.0 <= x <= 10.0 and 0.0 <= y <= 10.0)

  def test_straight_contour(self):
    contour = [(0, 0, True), (0, 64, True), (64, 64, True), (64, 0, True)]
    line = make_fallback_font.flatten(contour, 0.5)
    self.assertEqual(line, [(0.0, 0.0), (0.0, 32.0), (32.0, 32.0), (32.0, 0.0), (0.0, 0.0)])

  def test_26_6(self):
    self.assertEqual(make_fallback_font.to_26_6(1.0), 64)
    self.assertEqual(make_fallback_font.to_26_6(-0.5), -32)
    self.assertEqual(make_fallback_font.to_26_6(1.0 / 128.0), 1)


class FontFileTest(unittest.TestCase):

  def test_rejects_what_is_not_truetype(self):
    for data in (b'', b'OTTO' + bytes(20), struct.pack('>IH', 0x00010000, 1) + bytes(6)):
      with self.assertRaises(make_fallback_font.FontError):
        make_fallback_font.TrueTypeFont(data)

  def test_rejects_a_table_out_of_bounds(self):
    data = struct.pack('>IHHHH', 0x00010000, 1, 0, 0, 0) + struct.pack('>4sIII', b'head', 0, 28,
                                                                       1000)
    with self.assertRaises(make_fallback_font.FontError):
      make_fallback_font.TrueTypeFont(data)

  def test_check_without_the_font(self):
    with tempfile.TemporaryDirectory() as tmp:
      inc = pathlib.Path(tmp) / 'glyphs.inc'
      inc.write_text(f'// generator-sha256 {make_fallback_font.script_hash()}\n')
      with contextlib.redirect_stdout(io.StringIO()):
        self.assertEqual(make_fallback_font.main(['--check', '--output', str(inc)]), 0)
      inc.write_text('// generator-sha256 0000\n')
      with contextlib.redirect_stdout(io.StringIO()):
        self.assertEqual(make_fallback_font.main(['--check', '--output', str(inc)]), 1)

  def test_committed_table_matches_the_generator(self):
    inc = _ROOT / 'profiles' / 'mpo' / 'host' / 'font' / make_fallback_font.OUTPUT
    text = inc.read_text(encoding='utf-8')
    self.assertIn(f'// generator-sha256 {make_fallback_font.script_hash()}', text)
    self.assertIn(f'// font-sha256 {make_fallback_font.TTF_SHA256}', text)


if __name__ == '__main__':
  unittest.main()
