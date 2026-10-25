#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for profiles/mpo/tools/zstd_lite.py (no game data).

The compressed vectors below were produced by the reference zstd library
(python-zstandard 0.25.0, libzstd 1.5.7) from the synthetic payloads that
`_payload` regenerates, with one block flushed every few hundred bytes so
that later blocks reuse earlier tables (treeless literals, repeat modes).
Together they exercise raw and Huffman literals with 1 and 4 streams, direct
and FSE-compressed Huffman weights, predefined/RLE/FSE/repeat sequence
tables, 1- and 2-byte sequence counts and content checksums.
"""

import base64
import importlib.util
import pathlib
import random
import struct
import unittest


def _load_tool(name):
  # Loaded by path so the test also works under `python -I`.
  path = (pathlib.Path(__file__).resolve().parent.parent / 'tools' /
          f'{name}.py')
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


zstd_lite = _load_tool('zstd_lite')


def _lcg(seed):
  state = seed
  while True:
    state = (state * 1103515245 + 12345) & 0x7FFFFFFF
    yield state >> 8


def _payload(kind, size, seed=7):
  """Regenerates the synthetic input of a vector."""
  rnd = _lcg(seed)
  out = bytearray()
  if kind == 'two':
    while len(out) < size:
      out += bytes([65 + (next(rnd) & 1)])
  elif kind == 'four':
    while len(out) < size:
      out += bytes([65 + (next(rnd) & 3)])
  elif kind == 'skew8':
    while len(out) < size:
      out += bytes([int((next(rnd) % 64)**0.5) * 2 + 1])
  elif kind == 'tok':
    toks = [bytes(next(rnd) % 256 for _ in range(8)) for _ in range(16)]
    while len(out) < size:
      out += toks[next(rnd) % 16] + bytes([next(rnd) % 256])
  elif kind == 'ztok':
    toks = [bytes(next(rnd) % 256 for _ in range(6)) for _ in range(8)]
    while len(out) < size:
      out += toks[next(rnd) % 8] + b'Z'
  elif kind == 'aaa':
    while len(out) < size:
      out += b'A' * (next(rnd) % 50 + 1) + bytes([next(rnd) % 256]) * 4
  else:
    raise ValueError(kind)
  return bytes(out[:size])


_ZSTD_VECTORS = (
    ('aaa', 200, (
        'KLUv/QBoDAEAcEFPQZVBQYRB6EENQRZBDZAQ8A4AG+ABOKAgcAMfQBICAQAA'
    )),
    ('two', 200, (
        'KLUv/QRoRAEAQgsHBOAPATDVqh/8XBePxU2ENFb2nSUaC+951s0EHQIAuRZ23A1A'
        'AQEAAMDA7Ck='
    )),
    ('aaa', 200, (
        'KLUv/QBIxAEApAJBQU9PT09BlZWVlUGEhISEQejo6OhBDQ0NDUEWFhYWQUFBQUFB'
        'QUFBQUEGEACexrWiRf3RHhYBAAA='
    )),
    ('skew8', 200, (
        'KLUv/QBIjAIAgkwTjgEBAgMDBAMAK++QS+twEXx9tKLh3zcI1bcqWlhtTLctqXOG'
        'WaQwhIy+JdnfFiosp+xlRZurKm+aV+L/gByeogrO5FuinfJOWrSFpCgAAQAA'
    )),
    ('two', 1000, (
        'KLUv/QBorAIAht0RBOAPATAPAA8ADwA3EdJY2XeWaCy851k3E3QRvfLmsSZp1aof'
        '/FwXj0V7y6kJYtrl9BCGKTL78l9QQ4ZTtqVKFfjAKDqcjmwDAKc+EGkHW+4GoGQA'
        'AMOAAAAaAQDlA1SRBAEAAA=='
    )),
    ('ztok', 200, (
        'KLUv/QBoBAMAdAOSLWE1VA9a02Wc+HPJWqjoFg0oFlVPUJWQhFrKZe+8mtUDvY4A'
        'zj4vDX3AoKHfI43S67lVT1CVEYhwc9euGRBc83I7VAmvTQmYVK8At7sEy09RRaNW'
        'lQTAWTQKkS4pAQAA'
    )),
    ('tok', 200, (
        'KLUv/QBIvAQABAhzMnnxCExSMbKE1NjJZErcxWUki5ElXlN/w5+j40pdEgbBGjBV'
        'IR8d3bKiLGDruQO9jgDOPlVxc8mSLWE1VA+wfcCgod8jjdINVU9QlZCEqOjd058G'
        '62/q8nkxN95xzgmLMOj2mdvApeAk2Svi3ZvX4zH+2s0WDSgW02Wc+L13CgkEBUvI'
        'Gxcw7Gh3gWUVQqBS4ZlPKAEAAA=='
    )),
    ('ztok', 1000, (
        'KLUv/QRoPAQAZAOSLWE1VA9a02Wc+HPJWqjoFg0oFlVPUJWQhFrKZe+8mtUDvY4A'
        'zj4vDX3AoKHfI43S67kvDX0wqLHn2toMUExAheYBEbgFsDjrxgFtwNPVg+lFitAx'
        'RC5TgmN1z7ZCahSulhNtr8cn8oZWZa0m3UXbBITsOktI4P+UzShJ8oSBb5ZKzKsC'
        'VAIAAChoADBIIMUoOxE4BVJgBB/L0APQw/GiiB5zla2L6sM4V6a7VcXa3rEiITzM'
        'Y3pcHIhbOjTHmuCO4IMast9idZa7YQc8HTHeeDsBAAAZY4eQ'
    )),
    ('two', 1000, (
        'KLUv/QBI9AcA5AJCQkFCQUJBQUJCQkJBQkFBQkJCQkJBQUJBQUJCQUFCQUFCQkJB'
        'QkFBQkJBQkFCgI+owLVwQAKDjO0NEBDiYU0HrlMooF1z15ahJalckUxLuVa8FMM+'
        'zYt0r5YKTkMwVlHSPJJjouAitgQwIVy4Z0FVXgY0mKBulx+0tgyOpHJF9CCRd5x9'
        'am6vW/XsBmtDMLYoaT6SI459BicItAFSoGBFQWXyGSDwT3lY2b3TSzvvQ9Kwp1Wb'
        'ylP8CtYFhcZuCIFRtTb8HtKYTQZoTqYFR27aLvbTjWGo+54WOFDVegQlB81MK1tE'
        'mrmW7thtZm8IAVG1RvwekwDAS3dK4FIBAAA='
    )),
    ('four', 1000, (
        'KLUv/QBohAQARh8jBeAPaUAIIAAgACAAHoQEmOYLZ6bv/9bwpKN13b0nlCt22wd1'
        'ck5lQ0sCxAWXiavRZwTdNnaDRxYGSbw70cTmX6FSX3CxHYFVQJv6BTd/FGkvEaWp'
        'Gvg65QWCiCY9ZB1iKA/SkgPhY9M/y/UHZ8UoMY8YHuInExxPs2bOOBupwNWg8MKG'
        'zAfwY1RK8gQAVAQAR58hIAAgACAAMkyhzQxFVXxaDgnfdxeNPoHcca3f2OTP6eGo'
        'bguqEAV7za53nNwp7bys4xaRe25M9Qv49dobtyv/6jFQ07q4BsOFuw8DsFKQT68o'
        'IoyXzrfIgqV4OKlLyXmVYV/1jAQEmyWytEiNubblGcxkkrEDan8KWmgsZq1ayf7g'
        'WHZW+wYAAQAA'
    )),
    ('four', 3000, (
        'KLUv/QBo5AgACkBABAXgD2lACEEAQQBBANd/mbgafUbQbWM3eGRhkMS7E01s/hUq'
        '9QUX2xFYBbSpnwchAab5wpnp+781POloXXfvCeWK3fZBnZxT2dCSAHEBgipEZ8Uo'
        'MY8YHuInExxPs2bOOBupwNWg8MKGzAfwY1RK8tz8UaS9RJSmauDrlBcIIpr0kHWI'
        'oTxISw6Ej03/LAF91TMSsNfsesfJndLOyzpuEbnnxlS/gF+vvXG78q8eAzWti6sM'
        'U2gzQ1EVn5ZDwvfdRaNPIHdc6zc2+XN6OKrbASiA7s1vgpslsrRIjbm25RnMZJKx'
        'A2p/ClpoLGatWsn+4Fh2VvsOF+4+DMBKQT69oogwXjrfIguW4uGkLiXnVYYBAQAA'
        '1D7An6kBAAA='
    )),
    ('ztok', 3000, (
        'KLUv/QBoTAkANAOSLWE1VA9a02Wc+HPJWqjoFg0oFlVPUJWQhFrKZe+8mtUDvY4A'
        'zj4vDX3AoKHfI43S67mAmqjh62xtBkBIMFR09REIBVIY8VyMXefGZHEua5L2zyj/'
        'hvC5QI9a2SZCQ/dvDZbXo89GydWV1RRe2HyjiqmtdFFYhI7oj0Fto0NCpHkOqS2z'
        'LbFHUHbqLHBcU5CYJ3vV+0xIOw7ljRf/PWfgFLmKPogypPAcr6oExRBaVv3FWpEX'
        'ZPE/Einjm+aD8a6DqlSptb4zE2ge4z9drhog+ztCjm7DVfB/FBjYZvWWTIMpsFBt'
        '3phGazyiJhABKPAFm6RkuiJ+rLdXbaOiNzfJ3ZIOvUlpS02V0GpRybtuLDBSukCa'
        'MTn/6cKZXeUEUKD/jUAlOVQBrAMAAD54ABFoBIQI80yhNO8GyUnhROXqZUGYElcv'
        '4hF7s0rQDSmHxUneT1u0FQQErD7vskiHdclq4n6WPIreyFFotuo0uUHuj/N7Jl+b'
        'EVaRKTXAgCK6l6rRSNieQmTWWlLvQ/JLFg+0Vryt2c6UXpfZJ0CciGADAQAA'
    )),
    ('aaa', 3000, (
        'KLUv/QBoPAIAUsIHEuBpDVhG5wxKTLx+k9CEWHcLBU/PFxyL6HrDVi2bZB6gENge'
        '4PcMJplA0QEF4AHEABHYBC4BAdCAGYgDSoAb8AEkGgTsAQDwwEGhQSNB0kG5Qb1B'
        'AEE+QTJB8UFBTEFBMUEKQRtBHZgg/AAw42+SKCijrLSQDm9TDwRaSPfMGIdDBY8g'
        'jAIAogIJFNC1HDFqSkosIDaqCnFTUPOQwTQBT/QFqtnlOLhtktdQnLoBJODQl8Yb'
        'QAMzgQnYFMA5IIANAMgcymMDYidZQAgADVyBGxAWQAChAk0G/AEAMsIHEeAtB0gf'
        '8N4qAJMJYfqSICcmKNuS49RUVzF4ut8yDB/YACBjJj6RyaekIK021MtJ/CpNy4Yz'
        '0yV9XtAvFAIA+EFlQUGfQTBBYEFVQXFBsEENQUExQTeZQdtBwEGlQeMf6NDdDjBj'
        'JZAMJeVbXCk2AMHaIyxfRG1oBrSdeVEmhM869AIAEkMKFNBnDBSL2TTqhzfglElz'
        'v4TM8byBu7u+Ocx+kPFyc4PPxefTo9Wv/30kqDCubZCt1QGBBhNV/Ew0cRNVZmEH'
        'A4FX8chq4t9AsaN+yzkf0CBc5NeXoPzpkWiiAQEAAA=='
    )),
    ('aaa', 1000, (
        'KLUv/QBoPAIAUsIHEuBpDVhG5wxKTLx+k9CEWHcLBU/PFxyL6HrDVi2bZB6gENge'
        '4PcMJplA0QEF4AHEABHYBC4BAdCAGYgDSoAb8AEkGgTsAQDwwEGhQSNB0kG5Qb1B'
        'AEE+QTJB8UFBTEFBMUEKQRtBHZgg/AAw42+SKCijrLSQDm9TDwRaSPfMGIdDBY8g'
        'AQAA'
    )),
    ('two', 1000, (
        'KLUv/QBo3AIAxt4SBOAPATAQABAAEADiJkIaK/vOEo2F9zzrZoIO/0X0ypvHmqRV'
        'q37wc108Dh3ZveXUBDHtcnoIwxSZfQkAugs1ZDhlW6pUgQ+MosMJAwC78gFCSTuA'
        'cmEABQEAAA=='
    )),
)


def _vectors():
  for kind, size, chunks in _ZSTD_VECTORS:
    yield kind, size, base64.b64decode(''.join(chunks))


def _frame(blocks, content_size=None, checksum=None):
  """Builds a zstd frame from (block_type, body, size) tuples."""
  if content_size is None:
    descriptor = 0x00
    header = bytes([descriptor, 0x58])  # 2 MiB window, no content size
  else:
    descriptor = 0xA0  # single segment, 4-byte content size
    header = bytes([descriptor]) + struct.pack('<I', content_size)
  if checksum is not None:
    header = bytes([header[0] | 0x04]) + header[1:]
  out = struct.pack('<I', 0xFD2FB528) + header
  for index, (block_type, body, size) in enumerate(blocks):
    last = 1 if index == len(blocks) - 1 else 0
    value = last | (block_type << 1) | (size << 3)
    out += value.to_bytes(3, 'little') + body
  if checksum is not None:
    out += struct.pack('<I', checksum)
  return out


def _literals_header(literal_type, size):
  """Header of a raw (0) or RLE (1) literals section."""
  if size < 32:
    return bytes([literal_type | (size << 3)])
  if size < 4096:
    return bytes([literal_type | (1 << 2) | ((size & 0xF) << 4), size >> 4])
  return bytes([
      literal_type | (3 << 2) | ((size & 0xF) << 4), (size >> 4) & 0xFF,
      size >> 12
  ])


class VectorTest(unittest.TestCase):

  def test_reference_vectors_decode(self):
    for kind, size, compressed in _vectors():
      with self.subTest(kind=kind, size=size):
        self.assertEqual(zstd_lite.decompress(compressed, max_output=size),
                         _payload(kind, size))

  def test_output_cap_is_enforced(self):
    for kind, size, compressed in _vectors():
      with self.subTest(kind=kind, size=size):
        with self.assertRaises(zstd_lite.ZstdError):
          zstd_lite.decompress(compressed, max_output=size - 1)

  def test_every_truncation_is_rejected(self):
    for _, size, compressed in list(_vectors())[:4]:
      for length in range(len(compressed)):
        with self.subTest(size=size, length=length):
          with self.assertRaises(zstd_lite.ZstdError):
            zstd_lite.decompress(compressed[:length], max_output=size)

  def test_corruption_raises_only_zstd_error(self):
    rng = random.Random(1234)
    vectors = list(_vectors())
    for iteration in range(400):
      _, size, compressed = vectors[iteration % len(vectors)]
      data = bytearray(compressed)
      for _ in range(rng.randrange(1, 4)):
        position = rng.randrange(4, len(data))
        data[position] ^= 1 << rng.randrange(8)
      with self.subTest(iteration=iteration):
        try:
          out = zstd_lite.decompress(bytes(data), max_output=size)
        except zstd_lite.ZstdError:
          continue
        self.assertLessEqual(len(out), size)

  def test_random_noise_raises_only_zstd_error(self):
    rng = random.Random(99)
    for iteration in range(200):
      body = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64)))
      data = struct.pack('<I', 0xFD2FB528) + body
      with self.subTest(iteration=iteration):
        try:
          zstd_lite.decompress(data, max_output=1 << 16)
        except zstd_lite.ZstdError:
          pass


class HandBuiltFrameTest(unittest.TestCase):

  def test_raw_and_rle_blocks(self):
    frame = _frame([(0, b'abc', 3), (1, b'x', 5), (0, b'', 0)],
                   content_size=8)
    self.assertEqual(zstd_lite.decompress(frame, 8), b'abcxxxxx')

  def test_raw_and_rle_literals_without_sequences(self):
    for literal_type in (0, 1):
      for size in (0, 7, 31, 32, 4095, 4096, 70000):
        with self.subTest(literal_type=literal_type, size=size):
          data = bytes((index * 31) & 0xFF for index in range(size))
          if literal_type == 1:
            data = b'\x5a' * size
          literals = data if literal_type == 0 else b'\x5a'
          body = _literals_header(literal_type, size) + literals + b'\x00'
          # No content size: a single-segment frame would cap the block at
          # the content size, which these tiny compressed blocks exceed.
          frame = _frame([(2, body, len(body))])
          self.assertEqual(zstd_lite.decompress(frame, size), data)

  def test_frames_without_content_size_and_skippable_frames(self):
    skippable = struct.pack('<II', 0x184D2A5E, 3) + b'\x01\x02\x03'
    frame = _frame([(0, b'hello', 5)])
    data = skippable + frame + frame
    self.assertEqual(zstd_lite.decompress(data, 10), b'hellohello')

  def test_checksum_is_verified(self):
    good = zstd_lite.xxh64(b'hello') & 0xFFFFFFFF
    frame = _frame([(0, b'hello', 5)], content_size=5, checksum=good)
    self.assertEqual(zstd_lite.decompress(frame, 5), b'hello')
    bad = _frame([(0, b'hello', 5)], content_size=5, checksum=good ^ 1)
    with self.assertRaisesRegex(zstd_lite.ZstdError, 'checksum'):
      zstd_lite.decompress(bad, 5)

  def test_invalid_frames_are_rejected(self):
    cases = {
        'bad magic': b'\x00\x00\x00\x00\x00',
        'empty': b'',
        'reserved block type': _frame([(3, b'', 0)], content_size=0),
        'block larger than the window': _frame(
            [(2, b'\x08ab\x00', 4)], content_size=2),
        'content size mismatch': _frame([(0, b'ab', 2)], content_size=3),
        'dictionary': (struct.pack('<I', 0xFD2FB528) +
                       bytes([0x01, 0x58, 0x07]) + b'\x01\x00\x00'),
        'reserved header bit': (struct.pack('<I', 0xFD2FB528) +
                                bytes([0x08, 0x58]) + b'\x01\x00\x00'),
        'oversized block': _frame([(0, b'', 200000)]),
        'trailing garbage': _frame([(0, b'ab', 2)]) + b'\x01',
    }
    for name, data in cases.items():
      with self.subTest(name=name):
        with self.assertRaises(zstd_lite.ZstdError):
          zstd_lite.decompress(data, 1 << 20)


class Xxh64Test(unittest.TestCase):

  def test_known_values(self):
    # Reference values from the xxhash library (python-xxhash 3.x).
    cases = [
        (b'', 0, 0xEF46DB3751D8E999),
        (b'a', 0, 0xD24EC4F1A98C6E5B),
        (b'abc', 0, 0x44BC2CF5AD770999),
        (bytes(range(256)) * 3, 0, 0x8E03C838C596036F),
        (b'', 1, 0xD5AFBA1336A3BE4B),
        (bytes(range(256)) * 3, 1, 0xA80257374B99ADE3),
    ]
    for data, seed, expected in cases:
      with self.subTest(size=len(data), seed=seed):
        self.assertEqual(zstd_lite.xxh64(data, seed), expected)


if __name__ == '__main__':
  unittest.main()
