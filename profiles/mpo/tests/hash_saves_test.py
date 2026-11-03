#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for packaging/hash_saves.py (F22c): the smoke test's save check."""

import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

_ROOT = pathlib.Path(__file__).resolve().parents[3]


def _load(path):
  spec = importlib.util.spec_from_file_location(path.stem, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


hash_saves = _load(_ROOT / 'packaging' / 'hash_saves.py')


class HashSavesTest(unittest.TestCase):

  def setUp(self):
    self._tmp = tempfile.TemporaryDirectory()
    self.tmp = pathlib.Path(self._tmp.name)
    self.saves = self.tmp / 'ms0' / 'PSP' / 'SAVEDATA'
    (self.saves / 'ULUS10202001').mkdir(parents=True)
    (self.saves / 'ULUS10202001' / 'DATA.BIN').write_bytes(b'\x01' * 100)
    (self.saves / 'ULUS10202001' / 'PARAM.SFO').write_bytes(b'\x00PSF')
    self.snapshot = self.tmp / 'before.json'

  def tearDown(self):
    self._tmp.cleanup()

  def _run(self, *argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
      code = hash_saves.main([str(a) for a in argv])
    return code, out.getvalue()

  def test_unchanged(self):
    self.assertEqual(self._run('snapshot', self.saves, self.snapshot)[0], 0)
    code, out = self._run('compare', self.snapshot, self.saves)
    self.assertEqual(code, 0, out)
    self.assertIn('unchanged (2 files)', out)

  def test_added_removed_changed(self):
    self._run('snapshot', self.saves, self.snapshot)
    (self.saves / 'ULUS10202001' / 'DATA.BIN').write_bytes(b'\x02' * 100)
    (self.saves / 'ULUS10202001' / 'PARAM.SFO').unlink()
    (self.saves / 'ULUS10202002').mkdir()
    (self.saves / 'ULUS10202002' / 'DATA.BIN').write_bytes(b'new')
    code, out = self._run('compare', self.snapshot, self.saves)
    self.assertEqual(code, 1)
    self.assertIn('added: ULUS10202002/DATA.BIN', out)
    self.assertIn('removed: ULUS10202001/PARAM.SFO', out)
    self.assertIn('changed: ULUS10202001/DATA.BIN', out)

  def test_missing_folder_is_empty(self):
    missing = self.tmp / 'never'
    self._run('snapshot', missing, self.snapshot)
    self.assertEqual(self._run('compare', self.snapshot, missing)[0], 0)
    missing.mkdir()
    (missing / 'x').write_bytes(b'x')
    self.assertEqual(self._run('compare', self.snapshot, missing)[0], 1)

  def test_bad_snapshot(self):
    self.snapshot.write_text('{"format": "other"}', encoding='utf-8')
    self.assertEqual(self._run('compare', self.snapshot, self.saves)[0], 2)
    self.snapshot.write_text('not json', encoding='utf-8')
    self.assertEqual(self._run('compare', self.snapshot, self.saves)[0], 2)


if __name__ == '__main__':
  unittest.main()
