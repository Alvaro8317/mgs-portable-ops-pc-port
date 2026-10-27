#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the profile's analysis tools, with synthetic inputs only."""

import importlib.util
import io
import os
import pathlib
import struct
import tempfile
import unittest

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load(name):
  # Loaded by path so the test also works under `python -I`, which drops the
  # script directory from sys.path.
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


build_nid_table = _load('build_nid_table')
prx_imports = _load('prx_imports')
iso_lbn = _load('iso_lbn')

_HLE_SOURCE = '''
enum : u32 {
\tNID_MODULE_START = 0xD632ACDB,
};
const HLEFunction sceFoo[] =
{
\t{0X12345678, &WrapI_V<sceFooInit>, "sceFooInit", 'i', ""},
\t{0xABCDEF01, nullptr,              "sceFooMissing", '?', ""},
};

const HLEFunction sceBar[] =
{
\t{0x0BADF00D, &WrapV_V<sceBarRun>, "sceBarRun", 'v', ""},
};

const HLEFunction sceUnregistered[] =
{
\t{0x11111111, &WrapV_V<sceNope>, "sceNope", 'v', ""},
};

void Register_sceFoo() {
\tRegisterHLEModule("sceFoo", ARRAY_SIZE(sceFoo), sceFoo);
\tRegisterHLEModule("sceFoo_driver", ARRAY_SIZE(sceFoo), sceFoo);
\tRegisterHLEModule("sceBar", std::size(sceBar), sceBar);
}
'''


class BuildNidTableTest(unittest.TestCase):

  def test_parses_tables_libraries_and_syslib(self):
    with tempfile.TemporaryDirectory() as hle_dir:
      with open(os.path.join(hle_dir, 'sceFoo.cpp'), 'w',
                encoding='utf-8') as source:
        source.write(_HLE_SOURCE)
      rows, table_count = build_nid_table.parse_sources(hle_dir)
    self.assertEqual(table_count, 3)
    self.assertIn((0x12345678, 'sceFoo', 'sceFooInit', True, 'sceFoo.cpp'),
                  rows)
    self.assertIn((0x12345678, 'sceFoo_driver', 'sceFooInit', True,
                   'sceFoo.cpp'), rows)
    self.assertIn((0xABCDEF01, 'sceFoo', 'sceFooMissing', False, 'sceFoo.cpp'),
                  rows)
    self.assertIn((0x0BADF00D, 'sceBar', 'sceBarRun', True, 'sceFoo.cpp'), rows)
    self.assertIn((0xD632ACDB, 'syslib', 'module_start', True, 'sceFoo.cpp'),
                  rows)
    self.assertNotIn('sceNope', [row[2] for row in rows])

  def test_empty_directory_has_no_rows(self):
    with tempfile.TemporaryDirectory() as hle_dir:
      self.assertEqual(build_nid_table.parse_sources(hle_dir), ([], 0))


def _synthetic_prx(stub_entry_length=5):
  """Builds a tiny relocatable PRX with one import and one export library."""
  segment = bytearray(0xC0)
  # sceModuleInfo at vaddr 0: attr, minor, major, name, gp, ent/stub ranges.
  struct.pack_into('<HBB28s5I', segment, 0, 0, 1, 1, b'test_module', 0x8000,
                   0xA0, 0xB0, 0x60, 0x74)
  segment[0x40:0x48] = b'sceTest\0'
  struct.pack_into('<IHHBBHII', segment, 0x60, 0x40, 0x11, 0x4001,
                   stub_entry_length, 0, 2, 0x80, 0x90)
  struct.pack_into('<2I', segment, 0x80, 0x11111111, 0x22222222)
  struct.pack_into('<IHHBBHI', segment, 0xA0, 0, 0, 0x8000, 4, 1, 1, 0xB0)
  struct.pack_into('<4I', segment, 0xB0, 0xD632ACDB, 0xF01D73A7, 0x100, 0)
  segment_offset = 0x100
  header = bytearray(segment_offset)
  header[0:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
  # e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
  # e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx.
  struct.pack_into('<HHIIIIIHHHHHH', header, 16, 0xFFA0, 8, 1, 0, 52, 0, 0,
                   52, 32, 1, 0, 0, 0)
  # LOAD: p_paddr is the file offset of sceModuleInfo in a PRX.
  struct.pack_into('<8I', header, 52, 1, segment_offset, 0, segment_offset,
                   len(segment), len(segment), 5, 0x10)
  return bytes(header + segment)


class PrxImportsTest(unittest.TestCase):

  def _nids(self):
    return ({('sceTest', 0x11111111): ('sceTestRun', True)},
            {0x11111111: 'sceTestRun', 0xD632ACDB: 'module_start',
             0xF01D73A7: 'module_info'})

  def test_lists_imports_and_exports(self):
    module = prx_imports.Module(_synthetic_prx())
    self.assertEqual(module.name, 'test_module')
    self.assertEqual(list(module.imports()),
                     [('sceTest', 0x11, 0x4001, [0x11111111, 0x22222222], [])])
    self.assertEqual(list(module.exports()),
                     [('(module)', 0, 0x8000, [0xD632ACDB], [0xF01D73A7])])

  def test_describe_names_known_and_unknown_nids(self):
    with tempfile.TemporaryDirectory() as directory:
      path = os.path.join(directory, 'test.prx')
      with open(path, 'wb') as prx:
        prx.write(_synthetic_prx())
      lines = prx_imports.describe(path, self._nids(), summary=False)
    text = '\n'.join(lines)
    self.assertIn('2 functions, 0 variables (PPSSPP: 1 implemented, '
                  '0 unimplemented, 1 unknown)', text)
    self.assertIn('    sceTestRun', lines)
    self.assertIn('    sceTest_22222222', lines)
    self.assertIn('    0xD632ACDB module_start', lines)

  def test_rejects_truncated_modules(self):
    data = _synthetic_prx()
    for size in (0, 40, 100, 0x100 + 0x60):
      with self.subTest(size=size):
        with self.assertRaises(prx_imports.PrxError):
          module = prx_imports.Module(data[:size])
          list(module.imports())
          list(module.exports())

  def test_rejects_bad_stub_entry_length(self):
    module = prx_imports.Module(_synthetic_prx(stub_entry_length=0))
    with self.assertRaises(prx_imports.PrxError):
      list(module.imports())



def _dir_record(lbn, size, is_dir, name):
  """One ISO9660 directory record (both-endian fields), padded to even."""
  body = bytes([0])  # Extended attribute length.
  body += struct.pack('<I', lbn) + struct.pack('>I', lbn)
  body += struct.pack('<I', size) + struct.pack('>I', size)
  body += bytes(7)  # Date.
  body += bytes([2 if is_dir else 0, 0, 0])  # Flags, unit size, gap.
  body += struct.pack('<H', 1) + struct.pack('>H', 1)  # Volume sequence.
  body += bytes([len(name)]) + name
  if len(body) % 2 == 0:
    body += b'\x00'  # The length byte makes it odd: pad to an even record.
  return bytes([len(body) + 1]) + body


def _synthetic_iso():
  """Root (sector 20) with DATA.BIN (sector 30, 3 sectors) and SUB/ (21) with
  INNER.PMF (sector 40)."""
  sector = 2048
  image = bytearray(50 * sector)
  pvd = bytearray(sector)
  pvd[0] = 1
  pvd[1:6] = b'CD001'
  pvd[156:156 + 34] = _dir_record(20, sector, True, b'\x00')[:34]
  image[16 * sector:17 * sector] = pvd
  root = (_dir_record(20, sector, True, b'\x00') +
          _dir_record(20, sector, True, b'\x01') +
          _dir_record(30, 3 * sector, False, b'DATA.BIN;1') +
          _dir_record(21, sector, True, b'SUB'))
  image[20 * sector:20 * sector + len(root)] = root
  sub = (_dir_record(21, sector, True, b'\x00') +
         _dir_record(20, sector, True, b'\x01') +
         _dir_record(40, 100, False, b'INNER.PMF;1'))
  image[21 * sector:21 * sector + len(sub)] = sub
  return bytes(image)


class IsoLbnTest(unittest.TestCase):

  def test_names_the_file_of_each_sector(self):
    files = iso_lbn.list_files(io.BytesIO(_synthetic_iso()))
    self.assertEqual(files, [(30, 6144, 'DATA.BIN'), (40, 100, 'SUB/INNER.PMF')])
    self.assertEqual(iso_lbn.lookup(files, 30), ('DATA.BIN', 0))
    self.assertEqual(iso_lbn.lookup(files, 32), ('DATA.BIN', 4096))
    self.assertEqual(iso_lbn.lookup(files, 40), ('SUB/INNER.PMF', 0))
    self.assertEqual(iso_lbn.lookup(files, 33), (None, 0))

  def test_rejects_images_without_a_volume(self):
    with self.assertRaises(iso_lbn.IsoError):
      iso_lbn.list_files(io.BytesIO(bytes(40 * 2048)))
    with self.assertRaises(iso_lbn.IsoError):
      iso_lbn.list_files(io.BytesIO(bytes(100)))


if __name__ == '__main__':
  unittest.main()
