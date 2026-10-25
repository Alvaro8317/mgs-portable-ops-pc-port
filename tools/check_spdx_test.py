#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for check_spdx."""

import importlib.util
import pathlib
import subprocess
import tempfile
import unittest


def _load_check_spdx():
  # Loaded by path so the test also works under `python -I`, which drops the
  # script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / 'check_spdx.py'
  spec = importlib.util.spec_from_file_location('check_spdx', path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


check_spdx = _load_check_spdx()

GOOD_CPP = '// SPDX-License-Identifier: GPL-3.0-or-later\nint x;\n'
GOOD_SCRIPT = ('#!/usr/bin/env python3\n'
               '# SPDX-License-Identifier: GPL-3.0-or-later\n')


class IsCheckedTest(unittest.TestCase):

  def test_source_and_build_files_are_checked(self):
    for path in ('profiles/mpo/host/hle.cpp', 'tools/check_spdx.py',
                 'CMakeLists.txt', 'profiles/mpo/CMakeLists.txt',
                 '.github/workflows/ci.yml', 'profiles/mpo/config/x.toml'):
      with self.subTest(path=path):
        self.assertTrue(check_spdx.is_checked(path))

  def test_docs_and_foreign_code_are_not_checked(self):
    for path in ('README.md', 'LICENSE', 'docs/PLAN.md',
                 'framework/psprecomp/src/runtime.cpp',
                 'third_party/volk/volk.c', '.gitignore'):
      with self.subTest(path=path):
        self.assertFalse(check_spdx.is_checked(path))


class HasValidHeaderTest(unittest.TestCase):

  def test_tag_on_first_line(self):
    self.assertTrue(check_spdx.has_valid_header(GOOD_CPP))

  def test_tag_after_shebang(self):
    self.assertTrue(check_spdx.has_valid_header(GOOD_SCRIPT))

  def test_empty_file(self):
    self.assertTrue(check_spdx.has_valid_header(''))

  def test_missing_tag(self):
    self.assertFalse(check_spdx.has_valid_header('int x;\n'))

  def test_tag_too_late(self):
    text = '// Copyright\n// SPDX-License-Identifier: GPL-3.0-or-later\n'
    self.assertFalse(check_spdx.has_valid_header(text))

  def test_shebang_only(self):
    self.assertFalse(check_spdx.has_valid_header('#!/bin/sh\n'))

  def test_wrong_license(self):
    self.assertFalse(
        check_spdx.has_valid_header('// SPDX-License-Identifier: MIT\n'))


class FindViolationsTest(unittest.TestCase):

  def test_reports_only_bad_checked_files(self):
    with tempfile.TemporaryDirectory() as directory:
      root = pathlib.Path(directory)
      subprocess.run(['git', 'init', '-q'], cwd=root, check=True)
      files = {
          'good.cpp': GOOD_CPP,
          'bad.cpp': 'int x;\n',
          'tools/bad.py': 'import os\n',
          'README.md': 'no header needed\n',
          'third_party/lib/lib.c': 'int y;\n',
          'ignored/bad.cpp': 'int z;\n',
          '.gitignore': 'ignored/\n',
      }
      for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

      self.assertEqual(check_spdx.find_violations(root),
                       ['bad.cpp', 'tools/bad.py'])


if __name__ == '__main__':
  unittest.main()
