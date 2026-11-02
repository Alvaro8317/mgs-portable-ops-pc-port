#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for packaging/make_release.py (F22b): a release never carries the game."""

import importlib.util
import io
import pathlib
import struct
import tarfile
import tempfile
import unittest
import zipfile

_ROOT = pathlib.Path(__file__).resolve().parents[3]


def _load(path):
  spec = importlib.util.spec_from_file_location(path.stem, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


make_release = _load(_ROOT / 'packaging' / 'make_release.py')


def _mips_elf():
  header = bytearray(64)
  header[0:4] = b'\x7fELF'
  header[4] = 1  # 32-bit
  header[5] = 1  # little-endian
  struct.pack_into('<H', header, 18, 8)  # EM_MIPS
  return bytes(header)


def _x86_elf():
  header = bytearray(_mips_elf())
  struct.pack_into('<H', header, 18, 62)  # EM_X86_64
  return bytes(header)


def _iso():
  data = bytearray(0x8800)
  data[0x8001:0x8006] = b'CD001'
  return bytes(data)


class PathTest(unittest.TestCase):

  def test_game_paths(self):
    for path in ('x/game/ULUS-10202.iso', 'source/game/disc/a.bin', 'analysis/f21/x.txt',
                 'p/generated/mgp_main/a.cpp', 'framework/psprecomp/profiles/vcs/generated/a.cpp',
                 'EBOOT.BIN', 'PSP_GAME/SYSDIR/eboot.bin', 'stage/st02/st02.prx', 'a/PARAM.SFO',
                 'flash0/font/ltn0.pgf', 'f21/tramo1.input', 'stage/st02/_zar', 'x.ISO',
                 'dumps/a.txt', 'captures/frame.png'):
      self.assertIsNotNone(make_release.forbidden_path(path), path)

  def test_allowed_paths(self):
    for path in ('mpo-pc-port/source/docs/analysis/distribution.md', 'docs/analysis/rlc.md',
                 'source/profiles/mpo/config/recomp/stage_st02.hints.txt',
                 'source/profiles/mpo/host/hle/overlays.cpp', 'python/python314.zip',
                 'ffmpeg/lib/libavcodec.a', 'mpo_launcher.exe', 'source/docs/sessions/F21.md'):
      self.assertIsNone(make_release.forbidden_path(path), path)


class ContentTest(unittest.TestCase):

  def test_game_contents(self):
    self.assertIsNotNone(make_release.forbidden_content(b'~PSP' + bytes(60)))
    self.assertIsNotNone(make_release.forbidden_content(b'\0PBP' + bytes(60)))
    self.assertIsNotNone(make_release.forbidden_content(b'\0PSF' + bytes(60)))
    self.assertIsNotNone(make_release.forbidden_content(_mips_elf()))
    iso = _iso()
    self.assertIsNotNone(make_release.forbidden_content(iso[:64], iso[0x8000:0x8010]))

  def test_host_binaries_pass(self):
    self.assertIsNone(make_release.forbidden_content(_x86_elf()))
    self.assertIsNone(make_release.forbidden_content(b'MZ' + bytes(62)))
    self.assertIsNone(make_release.forbidden_content(b'# SPDX-License-Identifier: GPL'))


class ArchiveTest(unittest.TestCase):

  def _zip(self, root, members):
    path = root / 'release.zip'
    with zipfile.ZipFile(path, 'w') as z:
      for name, data in members.items():
        z.writestr(name, data)
    return path

  def test_clean_zip(self):
    with tempfile.TemporaryDirectory() as tmp:
      inner = io.BytesIO()
      with zipfile.ZipFile(inner, 'w') as z:
        z.writestr('os.pyc', b'\xcb\r\r\n')
      path = self._zip(pathlib.Path(tmp), {'mpo/mpo_launcher.exe': b'MZ' + bytes(100),
                                           'mpo/python/python314.zip': inner.getvalue(),
                                           'mpo/source/setup.py': b'# SPDX'})
      self.assertEqual(make_release.check_archive(path), [])

  def test_renamed_game_file_is_caught(self):
    with tempfile.TemporaryDirectory() as tmp:
      path = self._zip(pathlib.Path(tmp), {'mpo/data/innocent.bin': b'~PSP' + bytes(400),
                                           'mpo/data/disc.img': _iso(),
                                           'mpo/data/module.so': _mips_elf()})
      self.assertEqual(len(make_release.check_archive(path)), 3)

  def test_nested_zip_is_checked(self):
    with tempfile.TemporaryDirectory() as tmp:
      inner = io.BytesIO()
      with zipfile.ZipFile(inner, 'w') as z:
        z.writestr('EBOOT.BIN', b'~PSP')
      path = self._zip(pathlib.Path(tmp), {'mpo/python/python314.zip': inner.getvalue()})
      problems = make_release.check_archive(path)
      self.assertEqual(len(problems), 1)
      self.assertIn('python314.zip!EBOOT.BIN', problems[0])

  def test_tarball(self):
    with tempfile.TemporaryDirectory() as tmp:
      path = pathlib.Path(tmp) / 'source.tar.gz'
      with tarfile.open(path, 'w:gz') as t:
        for name, data in (('src/ok.cpp', b'// SPDX'), ('src/analysis/f21/x.txt', b'x')):
          info = tarfile.TarInfo(name)
          info.size = len(data)
          t.addfile(info, io.BytesIO(data))
      problems = make_release.check_archive(path)
      self.assertEqual(len(problems), 1)
      self.assertIn('analysis', problems[0])


class SourceTest(unittest.TestCase):

  def test_source_leaves_out_the_framework_profiles(self):
    if not (_ROOT / 'framework' / 'psprecomp' / '.git').exists():
      self.skipTest('the framework submodule is not checked out')
    files = make_release.source_files()
    self.assertIn('profiles/mpo/scripts/setup.py', files)
    self.assertIn('framework/psprecomp/CMakeLists.txt', files)
    self.assertFalse([f for f in files if f.startswith('framework/psprecomp/profiles/')])
    self.assertEqual([f for f in files if make_release.forbidden_path(f)], [])


if __name__ == '__main__':
  unittest.main()
