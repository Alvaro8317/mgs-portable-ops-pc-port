#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for profiles/mpo/tools/ge_frame_diff.py with synthetic images."""

import contextlib
import importlib.util
import io
import pathlib
import struct
import tempfile
import unittest
import zlib

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load_tool(name):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


ge_frame_diff = _load_tool('ge_frame_diff')


def _chunk(kind, body):
  return (struct.pack('>I', len(body)) + kind + body +
          struct.pack('>I', zlib.crc32(kind + body)))


def _paeth(a, b, c):
  p = a + b - c
  pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
  if pa <= pb and pa <= pc:
    return a
  return b if pb <= pc else c


def encode_png(width, height, pixels, color=6, filters=(0,)):
  """Encodes with the given filter types, cycling them row by row."""
  bpp = 4 if color == 6 else 3
  stride = width * bpp
  raw = bytearray()
  prev = bytes(stride)
  for y in range(height):
    line = pixels[y * stride:(y + 1) * stride]
    kind = filters[y % len(filters)]
    out = bytearray()
    for i in range(stride):
      left = line[i - bpp] if i >= bpp else 0
      upleft = prev[i - bpp] if i >= bpp else 0
      predictor = {0: 0, 1: left, 2: prev[i], 3: (left + prev[i]) >> 1,
                   4: _paeth(left, prev[i], upleft)}[kind]
      out.append((line[i] - predictor) & 0xFF)
    raw += bytes([kind]) + out
    prev = line
  return (b'\x89PNG\r\n\x1a\n' +
          _chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0,
                                      0, 0)) +
          _chunk(b'IDAT', zlib.compress(bytes(raw))) + _chunk(b'IEND', b''))


def pattern(width, height, seed=1):
  out = bytearray()
  for i in range(width * height):
    v = (i * 2654435761 + seed) & 0xFFFFFFFF
    out += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, 0xFF))
  return bytes(out)


class ReadPngTest(unittest.TestCase):

  def test_all_filters_round_trip(self):
    pixels = pattern(13, 9)
    for filters in ((0,), (1,), (2,), (3,), (4,), (0, 1, 2, 3, 4)):
      w, h, rgba = ge_frame_diff.read_png(encode_png(13, 9, pixels,
                                                     filters=filters))
      self.assertEqual((w, h), (13, 9))
      self.assertEqual(rgba, pixels, filters)

  def test_rgb_gets_opaque_alpha(self):
    rgb = bytes(range(2 * 2 * 3))
    _, _, rgba = ge_frame_diff.read_png(encode_png(2, 2, rgb, color=2,
                                                   filters=(4,)))
    self.assertEqual(rgba[0:4], bytes((0, 1, 2, 255)))
    self.assertEqual(rgba[12:16], bytes((9, 10, 11, 255)))

  def test_rejects_bad_files(self):
    good = encode_png(4, 4, pattern(4, 4))
    with self.assertRaises(ge_frame_diff.PngError):
      ge_frame_diff.read_png(b'not a png')
    corrupt = bytearray(good)
    corrupt[20] ^= 0xFF  # inside IHDR: CRC mismatch
    with self.assertRaises(ge_frame_diff.PngError):
      ge_frame_diff.read_png(bytes(corrupt))
    for size in range(len(good) - 1):
      with self.assertRaises(ge_frame_diff.PngError):
        ge_frame_diff.read_png(good[:size])
    palette = (b'\x89PNG\r\n\x1a\n' +
               _chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 3, 0, 0, 0)) +
               _chunk(b'IEND', b''))
    with self.assertRaises(ge_frame_diff.PngError):
      ge_frame_diff.read_png(palette)


class CompareTest(unittest.TestCase):

  def test_identical_and_cropped(self):
    a = (8, 4, pattern(8, 4))
    # The reference is wider (as a full-stride export): only the crop counts.
    wide = bytearray(pattern(10, 4, seed=7))
    for y in range(4):
      wide[y * 40:y * 40 + 32] = a[2][y * 32:(y + 1) * 32]
    result = ge_frame_diff.compare(a, (10, 4, bytes(wide)), 8, 4)
    self.assertEqual(result['differing'], 0)

  def test_counts_and_alpha(self):
    base = bytearray(pattern(4, 4))
    other = bytearray(base)
    other[0] ^= 0x01  # delta 1 at (0, 0)
    other[4 * 4 * 2 + 4 * 3 + 1] ^= 0x40  # big delta at (3, 2)
    other[3] = 0  # alpha only at (0, 0)
    result = ge_frame_diff.compare((4, 4, bytes(base)), (4, 4, bytes(other)),
                                   4, 4)
    self.assertEqual(result['differing'], 2)
    self.assertEqual(result['buckets']['1'], 1)
    self.assertEqual(result['buckets']['16+'], 1)
    self.assertEqual(result['box'], (0, 0, 3, 2))
    loose = ge_frame_diff.compare((4, 4, bytes(base)), (4, 4, bytes(other)),
                                  4, 4, tolerance=1)
    self.assertEqual(loose['differing'], 1)
    with_alpha = ge_frame_diff.compare((4, 4, bytes(base)),
                                       (4, 4, bytes(other)), 4, 4, alpha=True)
    self.assertEqual(with_alpha['max_delta'][3], 255)

  def test_too_small(self):
    with self.assertRaises(ge_frame_diff.PngError):
      ge_frame_diff.compare((4, 4, pattern(4, 4)), (4, 4, pattern(4, 4)), 8, 4)


class MainTest(unittest.TestCase):

  def test_cli(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      a = root / 'a.png'
      b = root / 'b.png'
      diff = root / 'diff.png'
      pixels = pattern(6, 5)
      a.write_bytes(encode_png(6, 5, pixels))
      b.write_bytes(encode_png(6, 5, pixels, filters=(2, 4)))
      with contextlib.redirect_stdout(io.StringIO()):
        self.assertEqual(ge_frame_diff.main(
            [str(a), str(b), '--width', '6', '--height', '5']), 0)
      changed = bytearray(pixels)
      changed[5] ^= 0x80
      b.write_bytes(encode_png(6, 5, bytes(changed)))
      out = io.StringIO()
      with contextlib.redirect_stdout(out):
        status = ge_frame_diff.main([str(a), str(b), '--width', '6',
                                     '--height', '5', '--diff-out', str(diff)])
      self.assertEqual(status, 1)
      self.assertIn('differing: 1 ', out.getvalue())
      w, h, rgba = ge_frame_diff.read_png(diff.read_bytes())
      self.assertEqual((w, h), (6, 5))
      self.assertEqual(rgba[4:8], bytes((255, 0, 0, 255)))
      with contextlib.redirect_stderr(io.StringIO()):
        self.assertEqual(ge_frame_diff.main([str(a), str(root / 'none.png')]),
                         2)


if __name__ == '__main__':
  unittest.main()
