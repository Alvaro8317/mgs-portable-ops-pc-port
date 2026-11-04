#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for setup.py and the versioned analysis hints (F22), without the game.

F24a: the settings (settings.toml) and the mpo_boot options they become.
"""

import argparse
import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

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

  def test_ppsspp_on_windows(self):
    folders = setup.ppsspp_windows_font_dirs({'ProgramFiles': 'C:\\Program Files',
                                              'ProgramW6432': 'C:\\Program Files',
                                              'LOCALAPPDATA': 'C:\\Users\\p\\AppData\\Local'})
    for folder in folders:
      self.assertEqual(folder.parts[-4:], ('PPSSPP', 'assets', 'flash0', 'font'))
    self.assertEqual(len(folders), 2, 'one folder per distinct root')

  def test_fonts_command(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      s = setup.Setup(argparse.Namespace(data=str(root / 'data'), fonts=None))
      if setup.find_fonts(None, None) is None:
        # Nothing on this machine: the `fonts` command says so (the launcher shows it).
        with self.assertRaises(setup.SetupError):
          s.step_fonts(required=True)
      self.assertFalse(s.status()['fonts'] and not s.fonts_installed())
      source = root / 'font'
      source.mkdir()
      for name in ('ltn0.pgf', 'jpn0.pgf', 'ltn1.pgf'):
        (source / name).write_bytes(b'pgf')
      s.args.fonts = str(source)
      s.step_fonts(required=True)
      self.assertTrue(s.fonts_installed())
      self.assertTrue(s.status()['fonts'])
      self.assertTrue((s.data / 'flash0' / 'font' / 'ltn1.pgf').is_file())


class StageLibraryTest(unittest.TestCase):

  def test_names_per_level(self):
    self.assertEqual(setup.library_name('st02', system='Linux'), 'stage_st02.so')
    self.assertEqual(setup.library_name('st02', quick=True, system='Linux'), 'stage_st02-O0.so')
    self.assertEqual(setup.library_name('st02', quick=True, system='Windows'),
                     'stage_st02-O0.dll')
    self.assertEqual(setup.library_name('title', system='Darwin'), 'stage_title.dylib')

  def test_status_counts_each_stage_once(self):
    with tempfile.TemporaryDirectory() as tmp:
      # Never the player's own settings.toml.
      args = argparse.Namespace(data=tmp, settings=str(pathlib.Path(tmp) / 'settings.toml'))
      s = setup.Setup(args)
      s.overlays.mkdir(parents=True)
      for name in ('stage_st02.so', 'stage_st02-O0.so', 'stage_st03-O0.so', 'stage_st04.so',
                   '.stage_st05.so.tmp'):
        (s.overlays / name).write_bytes(b'')
      status = s.status()
      self.assertEqual(status['stages_installed'], 3)
      self.assertEqual(status['stages_optimized'], 2)
      self.assertEqual(status['stages_quick_only'], 1)
      self.assertEqual(status['stages_total'], 92)
      self.assertEqual(status['settings'], setup.DEFAULT_SETTINGS)
      self.assertEqual(status['settings_notices'], [])
      self.assertEqual(status['settings_file'], args.settings)
      json.dumps(status)


class ConfigDirTest(unittest.TestCase):
  """The folder of input.toml, as mpo_boot's default_config_dir (save_paths.cpp)."""

  def test_windows(self):
    path = setup.default_config_dir({'APPDATA': r'C:\Users\p\AppData\Roaming'}, 'Windows')
    self.assertEqual(path.name, 'mpo-pc-port')
    self.assertTrue(str(path).startswith(r'C:\Users\p\AppData\Roaming'))

  def test_xdg(self):
    path = setup.default_config_dir({'XDG_CONFIG_HOME': '/cfg', 'HOME': '/home/p'}, 'Linux')
    self.assertEqual(path, pathlib.Path('/cfg/mpo-pc-port'))

  def test_relative_xdg_is_ignored(self):
    path = setup.default_config_dir({'XDG_CONFIG_HOME': 'cfg', 'HOME': '/home/p'}, 'Linux')
    self.assertEqual(path, pathlib.Path('/home/p/.config/mpo-pc-port'))

  def test_default_settings_file(self):
    with tempfile.TemporaryDirectory() as tmp:
      env = {'APPDATA': tmp, 'XDG_CONFIG_HOME': tmp}
      with mock.patch.dict(setup.os.environ, env):
        s = setup.Setup(argparse.Namespace(data=tmp))
      self.assertEqual(s.settings_path, pathlib.Path(tmp) / 'mpo-pc-port' / 'settings.toml')


class SettingsTest(unittest.TestCase):
  """settings.toml (F24a): read, checked, and turned into mpo_boot options."""

  def setUp(self):
    self._tmp = tempfile.TemporaryDirectory()
    self.tmp = pathlib.Path(self._tmp.name)
    self.path = self.tmp / 'settings.toml'

  def tearDown(self):
    self._tmp.cleanup()

  def load(self, text):
    self.path.write_bytes(text if isinstance(text, bytes) else text.encode('utf-8'))
    return setup.load_settings(self.path)

  def test_missing_file_is_the_defaults(self):
    settings, notices = setup.load_settings(self.tmp / 'nothing.toml')
    self.assertEqual(settings, setup.DEFAULT_SETTINGS)
    self.assertEqual(notices, [])
    self.assertEqual(setup.DEFAULT_SETTINGS, {'renderer': 'vulkan', 'render_scale': 1,
                                              'window_scale': 2, 'fullscreen': False,
                                              'language': 'auto'})

  def test_reads_what_the_launcher_writes(self):
    # launcher_core_test.cpp checks that format_settings writes exactly this.
    golden = _PROFILE / 'tests' / 'data' / 'settings_golden.txt'
    settings, notices = setup.load_settings(golden)
    self.assertEqual(notices, [])
    self.assertEqual(settings, {'renderer': 'vulkan', 'render_scale': 4, 'window_scale': 3,
                                'fullscreen': True, 'language': 'es'})
    # And as a Windows checkout may leave it.
    crlf = golden.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
    self.assertEqual(self.load(crlf), (settings, []))

  def test_every_valid_value(self):
    for scale in range(1, 9):
      settings, notices = self.load(f'render_scale = {scale}\nwindow_scale = {scale}\n')
      self.assertEqual(notices, [])
      self.assertEqual((settings['render_scale'], settings['window_scale']), (scale, scale))
    settings, notices = self.load('renderer = "soft"\nfullscreen = true\n')
    self.assertEqual(notices, [])
    self.assertEqual((settings['renderer'], settings['fullscreen']), ('soft', True))
    # F25d: the launcher's language.
    for language in ('auto', 'en', 'es'):
      settings, notices = self.load(f'language = "{language}"\n')
      self.assertEqual(notices, [])
      self.assertEqual(settings['language'], language)

  def test_invalid_values_fall_back_one_by_one(self):
    settings, notices = self.load('renderer = "opengl"\nrender_scale = 9\nwindow_scale = 0\n'
                                  'fullscreen = "yes"\n')
    self.assertEqual(settings, setup.DEFAULT_SETTINGS)
    self.assertEqual(len(notices), 4)
    self.assertIn('renderer = "opengl" is not valid ("vulkan" or "soft"): using "vulkan"',
                  notices[0])
    self.assertIn('render_scale = 9 is not valid (1 to 8): using 1', notices[1])
    self.assertIn('window_scale = 0 is not valid (1 to 8): using 2', notices[2])
    self.assertIn('fullscreen = "yes" is not valid (true or false): using false', notices[3])
    # F25d: a language the launcher does not have is "auto" (the system's).
    for text in ('"fr"', '"ES"', '"es_ES"', '""', 'true', '1', '["es"]'):
      settings, notices = self.load(f'language = {text}\n')
      self.assertEqual(settings, setup.DEFAULT_SETTINGS, text)
      self.assertEqual(len(notices), 1, text)
      self.assertIn(f'language = {text} is not valid ("auto" or "en" or "es"): using "auto"',
                    notices[0])
    # The good ones stay.
    settings, notices = self.load('render_scale = 4\nwindow_scale = 2.5\n')
    self.assertEqual((settings['render_scale'], settings['window_scale']), (4, 2))
    self.assertEqual(len(notices), 1)

  def test_types_toml_could_confuse(self):
    # true is a Python int; 2.0 and "2" are not integers; a table is not a value.
    for text in ('render_scale = true', 'render_scale = 2.0', 'render_scale = "2"',
                 'render_scale = [2]', 'fullscreen = 1', 'renderer = ["vulkan"]',
                 '[render_scale]\nvalue = 2', 'render_scale = 1979-05-27'):
      settings, notices = self.load(text + '\n')
      self.assertEqual(settings, setup.DEFAULT_SETTINGS, text)
      self.assertEqual(len(notices), 1, text)

  def test_unknown_keys_are_ignored(self):
    settings, notices = self.load('render_scale = 3\nshader = "crt"\n')
    self.assertEqual(settings['render_scale'], 3)
    self.assertEqual(notices, ["settings.toml: unknown setting 'shader', ignored"])

  def test_damaged_file_is_the_defaults(self):
    for data in (b'render_scale = \n', b'render_scale = 4\nrender_scale = 5\n',
                 b'\xff\xfe r\x00', b'[unclosed\n', b'\x00' * 64):
      settings, notices = self.load(data)
      self.assertEqual(settings, setup.DEFAULT_SETTINGS, data)
      self.assertEqual(len(notices), 1, data)
      self.assertIn('cannot be read', notices[0])
      self.assertIn('using the default settings', notices[0])

  def test_unreadable_file_is_the_defaults(self):
    # A folder where the file should be.
    self.path.mkdir()
    settings, notices = setup.load_settings(self.path)
    self.assertEqual(settings, setup.DEFAULT_SETTINGS)
    self.assertEqual(len(notices), 1)

  def test_long_values_are_shortened(self):
    _, notices = self.load('renderer = "' + 'x' * 200 + '"\n')
    self.assertLess(len(notices[0]), 160)
    self.assertIn('...', notices[0])

  def options(self, settings=None, extra=(), environ=None):
    return setup.settings_options(dict(setup.DEFAULT_SETTINGS, **(settings or {})), extra,
                                  environ or {})

  def test_options(self):
    self.assertEqual(self.options(), ['--renderer', 'vulkan', '--scale', '2', '--fullscreen', '0',
                                      '--render-scale', '1'])
    self.assertEqual(self.options({'render_scale': 8, 'window_scale': 4, 'fullscreen': True}),
                     ['--renderer', 'vulkan', '--scale', '4', '--fullscreen', '1',
                      '--render-scale', '8'])

  def test_render_scale_is_vulkan_only(self):
    # mpo_boot refuses --render-scale above 1 with softgpu.
    self.assertEqual(self.options({'renderer': 'soft', 'render_scale': 4}),
                     ['--renderer', 'soft', '--scale', '2', '--fullscreen', '0'])
    self.assertNotIn('--render-scale',
                     self.options({'render_scale': 4}, extra=['--renderer', 'soft']))

  def test_options_after_dashes_win(self):
    extra = ['--scale', '5', '--render-scale', '2', '--fullscreen', '1', '--renderer', 'vulkan']
    self.assertEqual(self.options({'render_scale': 6, 'renderer': 'soft'}, extra), [])
    # The last of a repeated option is the one mpo_boot keeps.
    options = self.options({'render_scale': 6},
                           ['--renderer', 'vulkan', '--renderer', 'soft'])
    self.assertNotIn('--render-scale', options)
    # Others pass through untouched and leave the settings alone.
    self.assertEqual(self.options({'window_scale': 3}, ['--filter', 'linear']),
                     ['--renderer', 'vulkan', '--scale', '3', '--fullscreen', '0',
                      '--render-scale', '1'])

  def test_environment_wins_over_the_file(self):
    self.assertEqual(self.options({'renderer': 'vulkan', 'render_scale': 4},
                                  environ={'MPO_RENDERER': 'soft'}),
                     ['--renderer', 'soft', '--scale', '2', '--fullscreen', '0'])
    # mpo_boot reads MPO_RENDER_SCALE itself when --render-scale is not given.
    self.assertNotIn('--render-scale', self.options({'render_scale': 4},
                                                    environ={'MPO_RENDER_SCALE': '2'}))
    self.assertIn('--render-scale', self.options(environ={'MPO_RENDERER': ''}))

  def play_setup(self, settings_text=None):
    data = self.tmp / 'data'
    iso = self.tmp / 'game.iso'
    iso.write_bytes(b'')
    if settings_text is not None:
      self.path.write_text(settings_text, encoding='utf-8')
    s = setup.Setup(argparse.Namespace(data=str(data), settings=str(self.path)))
    s.state['iso'] = str(iso)
    return s

  def test_play_command(self):
    s = self.play_setup('render_scale = 4\nwindow_scale = 4\nfullscreen = true\n')
    with mock.patch.dict(setup.os.environ, clear=False) as env:
      env.pop('MPO_RENDERER', None)
      env.pop('MPO_RENDER_SCALE', None)
      command = s.play_command(['--scale', '1', '--filter', 'linear'])
    self.assertEqual(command[0], str(s.mpo_boot))
    joined = ' '.join(command)
    self.assertIn('--window 1', joined)
    self.assertIn('--renderer vulkan --fullscreen 1 --render-scale 4', joined)
    self.assertTrue(joined.endswith('--scale 1 --filter linear'))
    self.assertEqual(command.count('--scale'), 1)

  def test_play_command_with_a_damaged_file(self):
    s = self.play_setup('render_scale = [\n')
    with mock.patch.dict(setup.os.environ, clear=False) as env:
      env.pop('MPO_RENDERER', None)
      env.pop('MPO_RENDER_SCALE', None)
      with mock.patch('sys.stdout') as out:
        command = s.play_command([])
    printed = ''.join(c.args[0] for c in out.write.call_args_list)
    self.assertIn('setup: settings: settings.toml cannot be read', printed)
    self.assertIn('--renderer vulkan --scale 2 --fullscreen 0 --render-scale 1',
                  ' '.join(command))

  def test_settings_command(self):
    self.path.write_text('window_scale = 99\n', encoding='utf-8')
    with mock.patch('sys.stdout') as out:
      code = setup.main(['settings', '--settings', str(self.path), '--json'])
    self.assertEqual(code, 0)
    report = json.loads(''.join(c.args[0] for c in out.write.call_args_list))
    self.assertEqual(report['settings'], setup.DEFAULT_SETTINGS)
    self.assertEqual(len(report['notices']), 1)
    self.assertEqual(report['file'], str(self.path))


class LockTest(unittest.TestCase):

  def test_one_install_at_a_time(self):
    with tempfile.TemporaryDirectory() as tmp:
      first = setup.Setup(argparse.Namespace(data=tmp))
      second = setup.Setup(argparse.Namespace(data=tmp))
      with first.lock():
        with self.assertRaises(setup.SetupError):
          with second.lock():
            pass
      with second.lock():
        pass


class ToolchainTest(unittest.TestCase):

  def test_platform(self):
    self.assertEqual(setup.toolchain_platform('Windows', 'AMD64'), 'windows-x86_64')
    self.assertIsNone(setup.toolchain_platform('Linux', 'x86_64'))
    self.assertIsNone(setup.toolchain_platform('Windows', 'ARM64'))

  def test_profile_pins_every_tool(self):
    path = _PROFILE / 'config' / 'toolchain.toml'
    tools = setup.load_toolchain(path, 'windows-x86_64')
    self.assertEqual(sorted(tools), ['cmake', 'llvm-mingw', 'ninja'])
    for name, spec in tools.items():
      self.assertTrue(spec['url'].startswith('https://'), name)
      self.assertIn(spec['version'], spec['url'], name)

  def test_archive_paths(self):
    setup.check_archive_members(['llvm/bin/clang.exe', 'ninja.exe'])
    for bad in ('../evil.dll', '/etc/passwd', 'C:/Windows/x.dll', 'a/../../b', '\\server\\x'):
      with self.assertRaises(setup.SetupError, msg=bad):
        setup.check_archive_members([bad])

  def _archive(self, root):
    archive = root / 'tool-1.0.zip'
    with zipfile.ZipFile(archive, 'w') as z:
      z.writestr('tool-1.0/bin/tool.exe', b'MZ')
    return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

  def test_download_checks_the_sha256(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      archive, sha256 = self._archive(root)
      s = setup.Setup(argparse.Namespace(data=str(root / 'data')))
      spec = {'url': archive.as_uri(), 'sha256': sha256, 'size': archive.stat().st_size}
      target = root / 'downloads' / 'tool.zip'
      s._download('tool', spec, target)
      self.assertEqual(target.read_bytes(), archive.read_bytes())
      s._unpack('tool', target, root / 'tool-1.0')
      self.assertTrue((root / 'tool-1.0' / 'tool-1.0' / 'bin' / 'tool.exe').is_file())
      bad = dict(spec, sha256='0' * 64)
      with self.assertRaises(setup.SetupError):
        s._download('tool', bad, root / 'downloads' / 'bad.zip')
      self.assertFalse((root / 'downloads' / 'bad.zip').exists())
      self.assertFalse((root / 'downloads' / 'bad.zip.part').exists())


  def test_unpack_lists_its_files(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      archive, _ = self._archive(root)
      s = setup.Setup(argparse.Namespace(data=str(root / 'data')))
      folder = root / 'tool-1.0'
      s._unpack('tool', archive, folder)
      self.assertEqual(setup.missing_toolchain_files(folder), [])
      # What an antivirus did to clang-23.exe on a player's Windows (F22c).
      (folder / 'tool-1.0' / 'bin' / 'tool.exe').unlink()
      self.assertEqual(setup.missing_toolchain_files(folder), ['tool-1.0/bin/tool.exe'])
      # Unpacked before F22c: no list, so it cannot be checked.
      (folder / setup._TOOLCHAIN_FILES).unlink()
      self.assertIsNone(setup.missing_toolchain_files(folder))

  def test_a_compiler_that_does_not_run(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = pathlib.Path(tmp)
      s = setup.Setup(argparse.Namespace(data=str(root / 'data')))
      folder = root / 'llvm-mingw-1'
      folder.mkdir()
      stamp = folder / '.mpo-sha256'
      stamp.write_text('0' * 64 + '\n', encoding='utf-8')
      with self.assertRaisesRegex(setup.SetupError, 'antivirus'):
        s._check_compiler(str(folder / 'bin' / 'clang.exe'), folder)
      self.assertFalse(stamp.exists(), 'the next Install unpacks it again')
      stamp.write_text('0' * 64 + '\n', encoding='utf-8')
      s._check_compiler(sys.executable, folder)  # `python --version` runs
      self.assertTrue(stamp.exists())


class BuildFolderTest(unittest.TestCase):

  def test_cache_from_another_source_is_dropped(self):
    with tempfile.TemporaryDirectory() as tmp:
      s = setup.Setup(argparse.Namespace(data=tmp))
      s.build.mkdir(parents=True)
      cache = s.build / 'CMakeCache.txt'
      (s.build / 'CMakeFiles').mkdir()
      (s.build / 'kept.o').write_bytes(b'')
      # The same source: nothing changes.
      cache.write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={setup._ROOT}\n', encoding='utf-8')
      s._forget_moved_source()
      self.assertTrue(cache.exists())
      # A release unpacked somewhere else (F22c).
      old = pathlib.Path(tmp) / 'Downloads' / 'release' / 'source'
      cache.write_text(f'FOO:STRING=1\nCMAKE_HOME_DIRECTORY:INTERNAL={old}\n', encoding='utf-8')
      self.assertTrue(setup.same_path(setup.cmake_cache_source(cache), old))
      s._forget_moved_source()
      self.assertFalse(cache.exists())
      self.assertFalse((s.build / 'CMakeFiles').exists())
      self.assertTrue((s.build / 'kept.o').exists(), 'objects stay')
      s._forget_moved_source()  # no cache: nothing to do

  def test_failure_shows_only_this_run(self):
    with tempfile.TemporaryDirectory() as tmp:
      s = setup.Setup(argparse.Namespace(data=tmp))
      fail = [sys.executable, '-c', 'import sys; print("new error"); sys.exit(3)']
      ok = [sys.executable, '-c', 'print("fine")']
      with self.assertRaises(setup.SetupError):
        s._run([sys.executable, '-c', 'import sys; print("old error"); sys.exit(1)'], 'step')
      s._run(ok, 'step')
      with self.assertRaises(setup.SetupError) as raised:
        s._run(fail, 'step')
      self.assertIn('new error', str(raised.exception))
      self.assertNotIn('old error', str(raised.exception))
      self.assertIn('old error', (s.logs / 'step.log').read_text(encoding='utf-8'))

  def test_missing_generated_files(self):
    with tempfile.TemporaryDirectory() as tmp:
      out = pathlib.Path(tmp) / 'st1403'
      self.assertIsNone(setup.missing_generated_files(out))
      (out / 'generated').mkdir(parents=True)
      names = ['generated_unit_0000.cpp', 'generated_unit_0001.cpp', 'stage_registry.cpp']
      (out / 'generate.json').write_text(
          json.dumps({'generated': {n: '0' * 64 for n in names}}), encoding='utf-8')
      for name in names:
        (out / 'generated' / name).write_text('', encoding='utf-8')
      self.assertEqual(setup.missing_generated_files(out), [])
      # Files that vanished after generating (an antivirus on a player's PC, F22c).
      (out / 'generated' / names[0]).unlink()
      self.assertEqual(setup.missing_generated_files(out), [names[0]])


  def test_stage_library_is_checked(self):
    with tempfile.TemporaryDirectory() as tmp:
      s = setup.Setup(argparse.Namespace(data=tmp, keep_objects=False, low_priority=False))
      built = s.build / 'overlays' / setup.library_name('st1402')
      objects = s._stage_objects('st1402')
      builds = []

      def fake_build(targets, log_name, low_priority=False):
        del log_name, low_priority
        builds.append((targets, objects.exists()))
        built.parent.mkdir(parents=True, exist_ok=True)
        built.write_bytes(b'library')

      s._cmake_build = fake_build
      # An object left by an interrupted compile (F22c) goes before building.
      objects.mkdir(parents=True)
      answers = ['exit code 0xC0000005: it crashed (access violation)', None]
      s._check_library = lambda library: answers.pop(0)
      self.assertEqual(s._build_stage('st1402'), built)
      self.assertEqual(builds, [(['mpo_overlay_st1402'], False)] * 2, 'built again from scratch')
      # A library that never loads is not left behind to be installed.
      s._check_library = lambda library: 'exit code 0xC0000005'
      with self.assertRaises(setup.SetupError) as raised:
        s._build_stage('st1402')
      self.assertIn('does not load', str(raised.exception))
      self.assertFalse(built.exists())


class ExitTest(unittest.TestCase):

  def test_describe_exit(self):
    self.assertEqual(setup.describe_exit(1, 'Windows'), 'exit code 1')
    self.assertEqual(setup.describe_exit(0xC0000135, 'Windows'),
                     'exit code 0xC0000135: a DLL it needs is missing')
    self.assertEqual(setup.describe_exit(0xC0000001, 'Windows'), 'exit code 0xC0000001')
    self.assertEqual(setup.describe_exit(-11, 'Linux'), 'killed by SIGSEGV')
    self.assertEqual(setup.describe_exit(-200, 'Linux'), 'killed by signal 200')


class ProfileHintsTest(unittest.TestCase):

  def test_header_round_trip(self):
    body = b'code 0x08804000 0x08804010\nseed 0x08804000 ghidra\n'
    data = generate.profile_hints_header('stage_x').encode() + body
    self.assertEqual(generate.strip_profile_header(data), body)
    self.assertEqual(generate.strip_profile_header(body), body)
    self.assertEqual(generate.strip_profile_header(data.replace(b'\n', b'\r\n')), body)

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
      # Read as a Windows checkout without .gitattributes would leave it, too.
      data = path.read_bytes().replace(b'\r\n', b'\n')
      header = generate.profile_hints_header(name).encode()
      self.assertTrue(data.startswith(header), path.name)
      directives = recomp_hints.parse_hints(data.decode('ascii'), path.name)
      kinds = {d[0] for d in directives}
      self.assertIn('code', kinds, path.name)
      self.assertLessEqual(kinds, {'code', 'seed', 'jumptable', 'patch'}, path.name)


if __name__ == '__main__':
  unittest.main()
