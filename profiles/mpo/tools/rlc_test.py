#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for rlc (and stage_inventory.functions) with synthetic data."""

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
rlc = _load('rlc')
stage_inventory = _load('stage_inventory')
synthetic_prx = _load('synthetic_prx')

info = synthetic_prx.info
R32, R26, HI16, LO16 = (rlc.R_MIPS_32, rlc.R_MIPS_26, rlc.R_MIPS_HI16,
                        rlc.R_MIPS_LO16)
MAIN_BASE = 0x08804000
STAGE_BASE = 0x08B8F900


def table(*entries):
  return b''.join(struct.pack('<II', offset, i) for offset, i in entries)


def decode_hi_lo(hi_word, lo_word):
  lo = lo_word & 0xFFFF
  lo = lo - 0x10000 if lo & 0x8000 else lo
  return (((hi_word & 0xFFFF) << 16) + lo) & 0xFFFFFFFF


def jal_target(word, pc):
  return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


class ParseTest(unittest.TestCase):

  def test_valid(self):
    entries = rlc.parse(
        table((0x40, info(R26, 0, 0)), (0x24, info(HI16, 0, 1)),
              (0x44, info(LO16, 0, 1)), (0x4, info(R32, 1, 0))))
    self.assertEqual(entries[1], rlc.Entry(0x24, HI16, 0, 1))
    self.assertEqual([e.type for e in entries], [R26, HI16, LO16, R32])

  def test_empty(self):
    self.assertEqual(rlc.parse(b''), [])

  def test_rejects(self):
    cases = {
        'partial entry': table((0, info(R32, 1, 0)))[:-1],
        'high bits': table((0, info(R32, 1, 0) | 0x01000000)),
        'R_MIPS_NONE': table((0, 0)),
        'GPREL16': table((0, info(7, 0, 0))),
        'misaligned': table((2, info(R32, 1, 0))),
        'HI16 at the end': table((0, info(HI16, 0, 1))),
        'HI16 then R26': table((0, info(HI16, 0, 1)), (4, info(R26, 0, 0)),
                               (8, info(LO16, 0, 1))),
        'HI16/LO16 bases differ': table((0, info(HI16, 0, 1)),
                                        (4, info(LO16, 0, 0))),
    }
    for label, data in cases.items():
      with self.subTest(case=label):
        with self.assertRaises(rlc.RlcError):
          rlc.parse(data)

  def test_several_hi16_share_one_lo16(self):
    entries = rlc.parse(
        table((0, info(HI16, 0, 1)), (4, info(HI16, 0, 1)),
              (8, info(LO16, 0, 1))))
    self.assertEqual(len(entries), 3)

  def test_random_tables_only_raise_rlc_error(self):
    rng = random.Random(7)
    for _ in range(3000):
      data = bytes(rng.randrange(256) for _ in range(8 * rng.randint(0, 6)))
      try:
        rlc.parse(data)
      except rlc.RlcError:
        pass

  def test_check_bounds(self):
    entries = rlc.parse(table((0xFC, info(R26, 0, 0))))
    rlc.check_bounds(entries, [0x100, 0x10], [0x1000, 0x10])
    for patch, target in (([0xFC, 0x10], [0x1000]), ([0x100], [])):
      with self.subTest(patch=patch, target=target):
        with self.assertRaises(rlc.RlcError):
          rlc.check_bounds(entries, patch, target)
    with self.assertRaises(rlc.RlcError):
      rlc.check_bounds(rlc.parse(table((0, info(R32, 2, 0)))), [4, 4], [4])


class ApplyTest(unittest.TestCase):
  """Semantics of PPSSPP ElfReader::LoadRelocations, checked by decoding."""

  def setUp(self):
    self.patch_bases = [0x08B00000, 0x08B10000]
    self.target_bases = [MAIN_BASE, MAIN_BASE + 0x1EED40]
    self.memory = bytearray(0x20000)

  def _put(self, segment, offset, word):
    position = self.patch_bases[segment] + offset - self.patch_bases[0]
    struct.pack_into('<I', self.memory, position, word)

  def _get(self, segment, offset):
    position = self.patch_bases[segment] + offset - self.patch_bases[0]
    return struct.unpack_from('<I', self.memory, position)[0]

  def _apply(self, *entries):
    rlc.apply(self.memory, self.patch_bases[0], rlc.parse(table(*entries)),
              self.patch_bases, self.target_bases)

  def test_r32(self):
    self._put(1, 8, 0x12340)
    self._apply((8, info(R32, 1, 0)))
    self.assertEqual(self._get(1, 8), MAIN_BASE + 0x12340)

  def test_r26(self):
    self._put(0, 0x40, 0x0C000000 | (0x123450 >> 2))
    self._apply((0x40, info(R26, 0, 0)))
    word = self._get(0, 0x40)
    self.assertEqual(word >> 26, 3)  # Still a jal.
    self.assertEqual(jal_target(word, self.patch_bases[0] + 0x40),
                     MAIN_BASE + 0x123450)

  def test_hi16_lo16_with_negative_lo(self):
    # lui 0x1 + addiu -0x8000: addend 0x8000 into mgp_main's data segment;
    # the result needs the lui carry adjustment.
    self._put(0, 0x24, 0x3C030001)
    self._put(0, 0x44, 0x24638000)
    self._apply((0x24, info(HI16, 0, 1)), (0x44, info(LO16, 0, 1)))
    address = decode_hi_lo(self._get(0, 0x24), self._get(0, 0x44))
    self.assertEqual(address, self.target_bases[1] + 0x8000)
    self.assertEqual(self._get(0, 0x24) & 0xFFFF0000, 0x3C030000)
    self.assertEqual(self._get(0, 0x44) & 0xFFFF0000, 0x24630000)

  def test_shared_lo16(self):
    self._put(0, 0x0, 0x3C030005)
    self._put(0, 0x4, 0x3C040005)
    self._put(0, 0x8, 0xAC62C120)
    self._apply((0x0, info(HI16, 0, 1)), (0x4, info(HI16, 0, 1)),
                (0x8, info(LO16, 0, 1)))
    expected = self.target_bases[1] + 0x4C120
    self.assertEqual(decode_hi_lo(self._get(0, 0x0), self._get(0, 0x8)),
                     expected)
    self.assertEqual(decode_hi_lo(self._get(0, 0x4), self._get(0, 0x8)),
                     expected)

  def test_lo16_patch_does_not_feed_hi16(self):
    # Originals are read first: the same result whatever the patch order.
    self._put(0, 0x0, 0x3C030000)
    self._put(0, 0x4, 0x2463FFFC)
    self._apply((0x0, info(HI16, 0, 0)), (0x4, info(LO16, 0, 0)))
    self.assertEqual(decode_hi_lo(self._get(0, 0x0), self._get(0, 0x4)),
                     MAIN_BASE - 4)

  def test_outside_memory(self):
    with self.assertRaises(rlc.RlcError):
      rlc.apply(bytearray(8), self.patch_bases[0],
                rlc.parse(table((8, info(R32, 0, 0)))), self.patch_bases,
                self.target_bases)


def _main_module():
  # 0x100 bytes of "code" plus a data segment, like a tiny mgp_main.
  return psp_prx.Prx(synthetic_prx.build([0] * 0x40, data=b'\0' * 0x40,
                                         bss=0x1000, name='mgp_main'))


def _stage_module(extra_words=(), relocations=None):
  # 0x00: jal (own function at 0x10), 0x08: jal mgp_main+0x80,
  # 0x10: lui/0x14: sw into mgp_main data +0x20, 0x18: jr ra.
  words = [0x0C000004, 0, 0x0C000020, 0, 0x3C030000, 0xAC620020, 0x03E00008, 0]
  words += list(extra_words)
  own = relocations if relocations is not None else [(0, info(R26, 0, 0)),
                                                    (0, info(R32, 1, 0))]
  return psp_prx.Prx(synthetic_prx.build(words, data=struct.pack('<I', 0x10),
                                         relocations=own))


_STAGE_RLC = table((0x08, info(R26, 0, 0)), (0x10, info(HI16, 0, 1)),
                   (0x14, info(LO16, 0, 1)))


class StageTest(unittest.TestCase):

  def test_check_counts(self):
    lines = rlc.check(_stage_module(), _STAGE_RLC, _main_module())
    self.assertIn('entries        3', lines)
    self.assertIn('call targets   1 distinct mgp_main addresses', lines)

  def test_check_rejects_overlap_with_own_relocations(self):
    stage = _stage_module(relocations=[(0x08, info(R26, 0, 0))])
    with self.assertRaises(rlc.RlcError):
      rlc.check(stage, _STAGE_RLC, _main_module())

  def test_check_rejects_reference_outside_mgp_main(self):
    stage = _stage_module()
    bad = table((0x08, info(R26, 0, 1)))  # +0x80 is past the data segment.
    with self.assertRaises(rlc.RlcError):
      rlc.check(stage, bad, psp_prx.Prx(synthetic_prx.build([0] * 0x40)))

  def test_check_rejects_table_that_does_not_match_the_code(self):
    stage, main = _stage_module(), _main_module()
    for label, bad in (('R26 on a lui', table((0x10, info(R26, 0, 0)))),
                       ('HI16 on a jal', table((0x08, info(HI16, 0, 1)),
                                               (0x14, info(LO16, 0, 1))))):
      with self.subTest(case=label):
        with self.assertRaises(rlc.RlcError):
          rlc.check(stage, bad, main)

  def test_relocate(self):
    stage, main = _stage_module(), _main_module()
    image = rlc.relocate(stage, _STAGE_RLC, main, STAGE_BASE, MAIN_BASE)
    word = lambda offset: struct.unpack_from('<I', image, offset)[0]
    self.assertEqual(jal_target(word(0x00), STAGE_BASE), STAGE_BASE + 0x10)
    self.assertEqual(jal_target(word(0x08), STAGE_BASE + 8), MAIN_BASE + 0x80)
    main_data = MAIN_BASE + main.load_segments[1].vaddr
    self.assertEqual(decode_hi_lo(word(0x10), word(0x14)), main_data + 0x20)
    data_vaddr = stage.load_segments[1].vaddr
    self.assertEqual(word(data_vaddr), STAGE_BASE + 0x10)
    with self.assertRaises(rlc.RlcError):
      rlc.relocate(stage, _STAGE_RLC, main, STAGE_BASE + 4, MAIN_BASE)


class FunctionHashTest(unittest.TestCase):

  def test_relocated_fields_do_not_change_the_hash(self):
    # Same code, different call targets into mgp_main.
    first = _stage_module()
    second_words = [0x0C000004, 0, 0x0C000123, 0, 0x3C030055, 0xAC627777,
                    0x03E00008, 0]
    second = psp_prx.Prx(synthetic_prx.build(
        second_words, data=struct.pack('<I', 0x10),
        relocations=[(0, info(R26, 0, 0)), (0, info(R32, 1, 0))]))
    entries = rlc.parse(_STAGE_RLC)
    first_functions = stage_inventory.functions(first, entries)
    second_functions = stage_inventory.functions(second, entries)
    self.assertEqual([(s, n) for s, n, _ in first_functions],
                     [(0, 0x10), (0x10, 0x10)])
    self.assertEqual([d for _, _, d in first_functions],
                     [d for _, _, d in second_functions])
    # Without the .rlc the mgp_main references are not masked.
    self.assertNotEqual(stage_inventory.functions(first)[0][2],
                        stage_inventory.functions(second)[0][2])


if __name__ == '__main__':
  unittest.main()
