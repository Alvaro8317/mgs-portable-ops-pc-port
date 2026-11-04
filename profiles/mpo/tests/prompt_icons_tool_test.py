#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests profiles/mpo/tools/prompt_icons.py (F25a) with synthetic data only.

A made-up TXP (one CLUT4 page of 32 x 16 with one frame) in a made-up QAR,
and a dump folder with PNGs named by made-up keys: the page's decoded RGBA
under one key, something else under another.
"""

import importlib.util
import io
import os
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load(name):
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


prompt_icons = _load('prompt_icons')
txp = prompt_icons.txp

_KEY = '000000000a1b2c3d4e5f6071'
_OTHER = '0000000011112222deadbeef'


def _txp_blob():
  header = struct.pack('<8I', 0, 0, 1, 0x20, 1, 0x30, 0, 0)
  texture = struct.pack('<HHHHII', (5 << 12) | 32, (4 << 12) | 16, 4, 0, 0x60,
                        0x160)
  frame = struct.pack('<IIIHHHHIHH', 0, 0x1234, 0, 12, 12, 2, 1, 0x20, 0, 0)
  frame += b'\0' * (0x30 - len(frame))
  pixels = bytes((i * 37 + 11) & 0xFF for i in range(256))
  clut = b''.join(struct.pack('<I', 0x80000000 | i * 0x0F0E0D) for i in range(16))
  return header + texture + frame + pixels + clut


def _qar(entries):
  data = bytearray()
  pairs = []
  for name, blob in entries:
    while len(data) % 0x80:
      data += b'\0'
    data += blob
    pairs.append((name, len(blob)))
  while len(data) % 0x80:
    data += b'\0'
  list_offset = len(data)
  data += struct.pack('<I', len(pairs))
  for _, size in pairs:
    data += struct.pack('<II', 0x14000000, size)
  for name, _ in pairs:
    data += name.encode('ascii') + b'\0'
  data += struct.pack('<I', list_offset)
  return bytes(data)


def _png(width, height, pixels, channels=4, filters=(0,)):
  """A PNG with each row filtered by filters[row % len(filters)]."""
  stride = width * channels
  rows = []
  previous = bytes(stride)
  for y in range(height):
    line = pixels[y * stride:(y + 1) * stride]
    kind = filters[y % len(filters)]
    out = bytearray()
    for i in range(stride):
      left = line[i - channels] if i >= channels else 0
      up = previous[i]
      corner = previous[i - channels] if i >= channels else 0
      predictor = {0: 0, 1: left, 2: up, 3: (left + up) // 2,
                   4: prompt_icons._paeth(left, up, corner)}[kind]
      out.append((line[i] - predictor) & 0xFF)
    rows.append(bytes((kind,)) + bytes(out))
    previous = line

  def chunk(tag, body):
    return (struct.pack('>I', len(body)) + tag + body +
            struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF))
  return (b'\x89PNG\r\n\x1a\n' +
          chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8,
                                     6 if channels == 4 else 2, 0, 0, 0)) +
          chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


class PngTest(unittest.TestCase):

  def test_filters(self):
    pixels = bytes((i * 13 + i // 7) & 0xFF for i in range(9 * 5 * 4))
    for filters in ((0,), (1,), (2,), (3,), (4,), (0, 1, 2, 3, 4)):
      w, h, out = prompt_icons.read_png(_png(9, 5, pixels, filters=filters))
      self.assertEqual((w, h, out), (9, 5, pixels), filters)

  def test_rgb(self):
    rgb = bytes(range(4 * 2 * 3))
    _, _, out = prompt_icons.read_png(_png(4, 2, rgb, channels=3, filters=(4,)))
    self.assertEqual(out[:4], rgb[:3] + b'\xff')
    self.assertEqual(len(out), 4 * 2 * 4)

  def test_writer_round_trip(self):
    pixels = bytes(range(256)) * 2
    with tempfile.TemporaryDirectory() as tmp:
      path = os.path.join(tmp, 'a.png')
      txp.write_png(path, 16, 8, pixels)
      with open(path, 'rb') as png:
        self.assertEqual(prompt_icons.read_png(png.read()), (16, 8, pixels))

  def test_rejects(self):
    good = _png(2, 2, bytes(16))
    for bad in (b'nope', good[:30], good.replace(b'IHDR', b'XHDR')):
      with self.assertRaises(prompt_icons.IconError):
        prompt_icons.read_png(bad)
    huge = bytearray(good)
    struct.pack_into('>I', huge, 16, 100000)
    with self.assertRaises(prompt_icons.IconError):
      prompt_icons.read_png(bytes(huge))


class MatchTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    root = pathlib.Path(self.tmp.name)
    self.stages = root / 'stages'
    (self.stages / 'r_main01').mkdir(parents=True)
    blob = _txp_blob()
    (self.stages / 'r_main01' / 'resident.qar').write_bytes(
        _qar([('lay_test_0.txp', blob), ('other.bin', b'x' * 5)]))
    parsed = txp.parse_txp(blob)
    page = txp.rgba(parsed, parsed.textures[0], 0)
    self.dump = root / 'dump'
    self.dump.mkdir()
    txp.write_png(str(self.dump / f'{_KEY}.png'), 32, 16, page)
    txp.write_png(str(self.dump / f'{_OTHER}.png'), 32, 16, bytes(32 * 16 * 4))
    (self.dump / 'textures.ini').write_text('[options]\n')
    (self.dump / 'broken000000000000000000.png').write_bytes(b'x')
    self.inventory = root / 'inventory.toml'
    self.inventory.write_text(
        'version = 1\n[[page]]\ntxp = "lay_test_0.txp"\ntexture = 0\n'
        'button = "cross"\n[[page]]\ntxp = "lay_missing_0.txp"\ntexture = 1\n'
        'button = "l"\n')

  def tearDown(self):
    self.tmp.cleanup()

  def test_match(self):
    entries, missing = prompt_icons.match(str(self.dump), str(self.stages),
                                          str(self.inventory))
    self.assertEqual(entries, [
        '[[icon]]\nkey = "000000000a1b2c3d4e5f6071"\nbutton = "cross"\n'
        'frame = [2, 1, 12, 12]\n'
        'source = "lay_test_0.txp texture 0 (r_main01/resident.qar)"\n'])
    self.assertEqual(len(missing), 1)
    self.assertIn('lay_missing_0.txp', missing[0])

  def test_known_keys_are_skipped(self):
    self.inventory.write_text(
        self.inventory.read_text() + f'[[icon]]\nkey = "{_KEY}"\n'
        'button = "cross"\n')
    entries, missing = prompt_icons.match(str(self.dump), str(self.stages),
                                          str(self.inventory))
    self.assertEqual(entries, [])
    self.assertEqual(len(missing), 1)

  def test_not_in_dump(self):
    os.remove(self.dump / f'{_KEY}.png')
    entries, missing = prompt_icons.match(str(self.dump), str(self.stages),
                                          str(self.inventory))
    self.assertEqual(entries, [])
    self.assertEqual(len(missing), 2)
    self.assertIn('not in the dump', missing[0])

  def test_main_appends(self):
    out = pathlib.Path(self.tmp.name) / 'prompts' / 'inventory.toml'
    out.parent.mkdir()
    out.write_text('version = 1\n')
    stderr = io.StringIO()
    old = sys.stderr
    sys.stderr = stderr
    try:
      code = prompt_icons.main(['match', str(self.dump), str(self.stages),
                                '--inventory', str(self.inventory),
                                '--out', str(out)])
    finally:
      sys.stderr = old
    self.assertEqual(code, 0)
    self.assertIn(f'key = "{_KEY}"', out.read_text())
    self.assertTrue(out.read_text().startswith('version = 1\n'))
    self.assertIn('1 new icons', stderr.getvalue())

  def test_bad_page(self):
    self.inventory.write_text('[[page]]\ntxp = "a.txp"\nbutton = "home"\n')
    with self.assertRaises(prompt_icons.IconError):
      prompt_icons.match(str(self.dump), str(self.stages), str(self.inventory))

  def test_real_inventory_pages(self):
    pages, known = prompt_icons._pages(prompt_icons._DEFAULT_INVENTORY)
    self.assertGreater(len(pages), 0)
    self.assertIn('0000000001e57e0adb2960d5', known)


if __name__ == '__main__':
  unittest.main()
