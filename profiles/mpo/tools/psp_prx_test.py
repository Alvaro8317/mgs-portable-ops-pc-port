#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for psp_prx with synthetic modules (no game data)."""

import importlib.util
import pathlib
import random
import struct
import unittest


def _load(name):
  # Loaded by path so the test also works under `python -I`.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


psp_prx = _load('psp_prx')
synthetic_prx = _load('synthetic_prx')

_NIDS = [0x11111111, 0x22222222, 0x33333333]


def _module(**kwargs):
  arguments = dict(text_words=[0x03E00008, 0, 0x0C000000, 0],
                   data=b'\1\2\3\4' * 4, bss=0x20,
                   relocations=[(8, synthetic_prx.info(4, 0, 0)),
                                (0, synthetic_prx.info(2, 1, 0))],
                   imports=[('ThreadManForUser', _NIDS[:2]),
                            ('sceSuspendForUser', _NIDS[2:])],
                   name='mgp_stage', gp=0x8123)
  arguments.update(kwargs)
  return synthetic_prx.build(**arguments)


class ParseTest(unittest.TestCase):

  def test_fields(self):
    prx = psp_prx.Prx(_module())
    self.assertEqual(prx.module_info.name, 'mgp_stage')
    self.assertEqual(prx.module_info.gp, 0x8123)
    self.assertEqual((prx.module_info.major, prx.module_info.minor), (1, 1))
    self.assertEqual(len(prx.load_segments), 2)
    self.assertEqual(prx.load_segments[1].memsz - prx.load_segments[1].filesz,
                     0x20)
    self.assertEqual([(l.name, l.function_nids) for l in prx.imports],
                     [('ThreadManForUser', _NIDS[:2]),
                      ('sceSuspendForUser', _NIDS[2:])])
    self.assertEqual(len(prx.exports), 1)
    export = prx.exports[0]
    self.assertEqual(export.name, '')
    self.assertEqual(
        [psp_prx.SYSLIB_NIDS[n] for n in export.function_nids],
        ['module_start', 'module_stop'])
    self.assertEqual(export.variable_nids, [0xF01D73A7])
    self.assertEqual(prx.relocations, [
        psp_prx.Relocation(8, 0x004), psp_prx.Relocation(0, 0x102)
    ])
    self.assertEqual(prx.section('.text').size, 16)
    self.assertIsNone(prx.section('.data'))

  def test_image_places_segments_and_zeroes_bss(self):
    prx = psp_prx.Prx(_module())
    image = prx.image()
    seg1 = prx.load_segments[1]
    self.assertEqual(len(image), seg1.vaddr + seg1.memsz)
    self.assertEqual(struct.unpack_from('<I', image, 0)[0], 0x03E00008)
    self.assertEqual(image[seg1.vaddr:seg1.vaddr + 16], b'\1\2\3\4' * 4)
    self.assertEqual(image[seg1.vaddr + 16:], b'\0' * 0x20)

  def test_module_without_imports(self):
    prx = psp_prx.Prx(_module(imports=[], relocations=[]))
    self.assertEqual(prx.imports, [])
    self.assertEqual(prx.relocations, [])


class RejectTest(unittest.TestCase):

  def _assert_rejected(self, data):
    with self.assertRaises(psp_prx.PrxError):
      psp_prx.Prx(data)

  def _patched(self, offset, fmt, value):
    data = bytearray(_module())
    struct.pack_into(fmt, data, offset, value)
    return bytes(data)

  def test_every_truncation(self):
    data = _module()
    for length in range(len(data)):
      with self.subTest(length=length):
        self._assert_rejected(data[:length])

  def test_bad_header_fields(self):
    self._assert_rejected(b'\x7fELX' + _module()[4:])
    self._assert_rejected(self._patched(5, '<B', 2))  # Big endian.
    self._assert_rejected(self._patched(16, '<H', 2))  # ET_EXEC.
    self._assert_rejected(self._patched(18, '<H', 3))  # Not MIPS.
    self._assert_rejected(self._patched(44, '<H', 0))  # No program headers.
    self._assert_rejected(self._patched(44, '<H', 1000))

  def test_segment_out_of_bounds(self):
    # filesz of the first LOAD segment (program header at 52).
    self._assert_rejected(self._patched(52 + 16, '<I', 0x7FFFFFF0))

  def test_compressed_relocations(self):
    self._assert_rejected(self._patched(52 + 64, '<I', 0x700000A1))

  def test_relocation_size_not_multiple_of_8(self):
    self._assert_rejected(self._patched(52 + 64 + 16, '<I', 12))

  def test_bad_import_table(self):
    prx = psp_prx.Prx(_module())
    info = prx.module_info.file_offset
    # stub_end below stub_top.
    self._assert_rejected(self._patched(info + 48, '<I', 0))
    # Library name pointer outside the file.
    stub = prx.file_offset(prx.module_info.stub_top)
    self._assert_rejected(self._patched(stub, '<I', 0x7FFFFF00))
    # Entry length below the minimum.
    self._assert_rejected(self._patched(stub + 8, '<B', 1))

  def test_random_corruption_only_raises_prx_error(self):
    data = _module()
    rng = random.Random(1234)
    for _ in range(2000):
      mutated = bytearray(data)
      for _ in range(rng.randint(1, 8)):
        mutated[rng.randrange(len(mutated))] = rng.randrange(256)
      try:
        psp_prx.Prx(bytes(mutated))
      except psp_prx.PrxError:
        pass


if __name__ == '__main__':
  unittest.main()
