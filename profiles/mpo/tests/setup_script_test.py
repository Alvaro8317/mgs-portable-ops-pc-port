#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for setup.py and the versioned analysis hints (F22), without the game."""

import importlib.util
import pathlib
import tempfile
import unittest

_PROFILE = pathlib.Path(__file__).resolve().parent.parent


def _load(path):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(path.stem, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


setup = _load(_PROFILE / 'scripts' / 'setup.py')
generate = _load(_PROFILE / 'scripts' / 'generate.py')
build_overlays = _load(_PROFILE / 'scripts' / 'build_overlays.py')
recomp_hints = _load(_PROFILE / 'tools' / 'recomp_hints.py')


class OrderTest(unittest.TestCase):

  def test_natural_key(self):
    names = ['st10', 'st02', 'st1', 'st0204', 'st02x']
    self.assertEqual(sorted(names, key=setup.natural_key),
                     ['st1', 'st02', 'st02x', 'st10', 'st0204'])

  def test_stage_order(self):
    with tempfile.TemporaryDirectory() as tmp:
      order = pathlib.Path(tmp) / 'order.toml'
      order.write_text('first = ["title", "st04vo", "missing"]\n'
                       'last_prefixes = ["n", "gs"]\n', encoding='utf-8')
      stages = ['n01', 'st10', 'gsmenu', 'st04vo', 'st02', 'title', 'ending']
      self.assertEqual(setup.stage_order(stages, order),
                       ['title', 'st04vo', 'ending', 'st02', 'st10', 'gsmenu', 'n01'])

  def test_profile_order_starts_with_the_boot(self):
    order = setup.stage_order(['st02', 'init', 'n01', 'st01t1', 'r_main01'],
                              _PROFILE / 'config' / 'build_order.toml')
    self.assertEqual(order, ['init', 'r_main01', 'st01t1', 'st02', 'n01'])


class DataDirTest(unittest.TestCase):

  def test_windows(self):
    path = setup.default_data_dir({'LOCALAPPDATA': r'C:\Users\p\AppData\Local'}, 'Windows')
    self.assertEqual(path.parts[-2:], ('mpo-pc-port', 'install'))
    self.assertTrue(str(path).startswith(r'C:\Users\p\AppData\Local'))

  def test_xdg(self):
    path = setup.default_data_dir({'XDG_DATA_HOME': '/data/home'}, 'Linux')
    self.assertEqual(path, pathlib.Path('/data/home/mpo-pc-port/install'))

  def test_home_fallback(self):
    path = setup.default_data_dir({}, 'Linux')
    self.assertEqual(path, pathlib.Path.home() / '.local/share/mpo-pc-port/install')


class RuntimeKeyTest(unittest.TestCase):

  def test_depends_on_compiler_and_level(self):
    key = setup.runtime_key('/usr/bin/clang++', '2')
    self.assertEqual(key, setup.runtime_key('/usr/bin/clang++', '2'))
    self.assertNotEqual(key, setup.runtime_key('/usr/bin/clang++', '1'))
    self.assertNotEqual(key, setup.runtime_key('/usr/bin/g++', '2'))


class FontsTest(unittest.TestCase):

  def test_next_to_the_iso(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      fonts = root / 'flash0' / 'font'
      fonts.mkdir(parents=True)
      iso = root / 'game.iso'
      self.assertNotEqual(setup.find_fonts(None, str(iso)), fonts)
      for name in ('ltn0.pgf', 'jpn0.pgf'):
        (fonts / name).write_bytes(b'')
      self.assertEqual(setup.find_fonts(None, str(iso)), fonts)

  def test_explicit_folder_must_hold_them(self):
    with tempfile.TemporaryDirectory() as tmp:
      with self.assertRaises(setup.SetupError):
        setup.find_fonts(tmp, None)


class ProfileHintsTest(unittest.TestCase):

  def test_header_round_trip(self):
    body = b'code 0x08804000 0x08804010\nseed 0x08804000 ghidra\n'
    data = generate.profile_hints_header('stage_x').encode() + body
    self.assertEqual(generate.strip_profile_header(data), body)
    self.assertEqual(generate.strip_profile_header(body), body)

  def test_every_corpus_has_its_hints(self):
    supported = _PROFILE / 'config' / 'supported.toml'
    stages = build_overlays.all_stages(supported)
    kept, _ = build_overlays.distinct_stages(stages, supported,
                                             _PROFILE / 'config' / 'stages.toml', generate)
    names = ['mgp_main', 'sound2', 'kjfs', 'zlibdec'] + [f'stage_{s}' for s in kept]
    missing = [n for n in names if not generate.profile_hints_path(n).is_file()]
    self.assertEqual(missing, [])
    extra = sorted({p.name[:-len('.hints.txt')]
                    for p in (_PROFILE / 'config' / 'recomp').glob('*.hints.txt')} - set(names))
    self.assertEqual(extra, [])

  def test_versioned_hints_parse(self):
    files = sorted((_PROFILE / 'config' / 'recomp').glob('*.hints.txt'))
    self.assertTrue(files)
    for path in files:
      name = path.name[:-len('.hints.txt')]
      data = path.read_bytes()
      header = generate.profile_hints_header(name).encode()
      self.assertTrue(data.startswith(header), path.name)
      directives = recomp_hints.parse_hints(data.decode('ascii'), path.name)
      kinds = {d[0] for d in directives}
      self.assertIn('code', kinds, path.name)
      self.assertLessEqual(kinds, {'code', 'seed', 'jumptable', 'patch'}, path.name)


if __name__ == '__main__':
  unittest.main()
