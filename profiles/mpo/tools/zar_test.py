#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for zar with synthetic archives (no game data)."""

import importlib.util
import pathlib
import random
import struct
import unittest
import zlib


def _load(name):
  # Loaded by path so the test also works under `python -I`.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


zar = _load('zar')


def build_dar(entries):
  """Builds a DAR with the layout documented in docs/formats/rlc.md."""
  out = bytearray(struct.pack('<I', len(entries)))
  for name, data in entries:
    out += name + b'\0'
    while len(out) % 4:
      out.append(0)
    out += struct.pack('<I', len(data))
    while len(out) % 16:
      out.append(0)
    out += data + b'\0'
  return bytes(out)


def build_zar(dar, declared=None):
  size = len(dar) if declared is None else declared
  return struct.pack('<I', size) + zlib.compress(dar)


_ENTRIES = [
    (b'data.cnf', b'.nocache\n.cache\nx.rlc\n'),
    (b'cache.dar', b''),  # Empty data still gets its NUL byte.
    (b'abcdefg.gcx', bytes(range(37))),
    (b'x.rlc', struct.pack('<4I', 0x40, 4, 0x24, 0x10005)),
]


class DarTest(unittest.TestCase):

  def test_round_trip(self):
    dar = build_dar(_ENTRIES)
    entries = zar.parse_dar(dar)
    self.assertEqual([e.name for e in entries],
                     [name.decode() for name, _ in _ENTRIES])
    for entry, (_, data) in zip(entries, _ENTRIES):
      self.assertEqual(entry.offset % 16, 0)
      self.assertEqual(dar[entry.offset:entry.offset + entry.size], data)
    self.assertEqual(zar.read_entry(dar, entries, 'x.rlc'), _ENTRIES[3][1])
    with self.assertRaises(zar.ZarError):
      zar.read_entry(dar, entries, 'missing.rlc')

  def test_nested_dar(self):
    inner = build_dar([(b'a.mdp', b'\x11' * 5)])
    outer = build_dar([(b'cache.dar', inner)])
    entries = zar.parse_dar(outer)
    blob = zar.read_entry(outer, entries, 'cache.dar')
    self.assertEqual(zar.parse_dar(blob)[0].name, 'a.mdp')

  def test_empty_dar(self):
    self.assertEqual(zar.parse_dar(struct.pack('<I', 0)), [])

  def test_every_truncation(self):
    dar = build_dar(_ENTRIES)
    for length in range(len(dar)):
      with self.subTest(length=length):
        with self.assertRaises(zar.ZarError):
          zar.parse_dar(dar[:length])

  def test_rejects(self):
    good = build_dar(_ENTRIES)
    cases = {
        'trailing bytes': good + b'\0',
        'count too large': struct.pack('<I', 1 << 20) + good[4:],
        'missing NUL after data': build_dar(_ENTRIES)[:-1] + b'X',
        'duplicate': build_dar([(b'a.bin', b'1'), (b'a.bin', b'2')]),
        'empty name': build_dar([(b'', b'1')]),
        'slash': build_dar([(b'../x', b'1')]),
        'backslash': build_dar([(b'a\\b', b'1')]),
        'dotdot': build_dar([(b'..', b'1')]),
        'control char': build_dar([(b'a\nb', b'1')]),
        'unterminated name': struct.pack('<I', 1) + b'A' * 300,
    }
    oversized = bytearray(build_dar([(b'a.bin', b'1234')]))
    struct.pack_into('<I', oversized, 12, 0xFFFFFFF0)
    cases['size out of bounds'] = bytes(oversized)
    for label, data in cases.items():
      with self.subTest(case=label):
        with self.assertRaises(zar.ZarError):
          zar.parse_dar(data)

  def test_random_corruption_only_raises_zar_error(self):
    dar = build_dar(_ENTRIES)
    rng = random.Random(99)
    for _ in range(2000):
      mutated = bytearray(dar)
      for _ in range(rng.randint(1, 4)):
        mutated[rng.randrange(len(mutated))] = rng.randrange(256)
      try:
        zar.parse_dar(bytes(mutated))
      except zar.ZarError:
        pass


class ZarTest(unittest.TestCase):

  def test_round_trip(self):
    dar = build_dar(_ENTRIES)
    self.assertEqual(zar.decompress(build_zar(dar)), dar)

  def test_size_mismatch(self):
    dar = build_dar(_ENTRIES)
    for declared in (len(dar) - 1, len(dar) + 1, 0):
      with self.subTest(declared=declared):
        with self.assertRaises(zar.ZarError):
          zar.decompress(build_zar(dar, declared))

  def test_limit(self):
    dar = b'\0' * 4096
    with self.assertRaises(zar.ZarError):
      zar.decompress(build_zar(dar), max_size=1024)
    # A stream that inflates beyond its header stops at header + 1 bytes.
    with self.assertRaises(zar.ZarError):
      zar.decompress(build_zar(b'\0' * (1 << 22), declared=16))

  def test_corrupt_streams(self):
    good = build_zar(build_dar(_ENTRIES))
    cases = {
        'short header': good[:3],
        'truncated stream': good[:len(good) // 2],
        'trailing data': good + b'junk',
        'not zlib': good[:4] + b'\xff' * 32,
    }
    for label, data in cases.items():
      with self.subTest(case=label):
        with self.assertRaises(zar.ZarError):
          zar.decompress(data)


if __name__ == '__main__':
  unittest.main()
