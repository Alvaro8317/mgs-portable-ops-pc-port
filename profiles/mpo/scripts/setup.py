#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the port on the player's machine, from their ISO to a playable game (F22).

The port never ships the game's code, translated or not (CLAUDE.md §1): this
script turns the player's own ISO into the native game, on their machine, and
is what the launcher runs. Every step is resumable; running it again only does
what is missing.

  install  1. tools: configures the build and compiles the installer (`mpo`)
              and the recompiler (`psp_recomp`).
           2. disc: `mpo --install <iso>` validates the ISO (disc ID and the
              SHA-256 of every file) and decrypts EBOOT.BIN into <data>/game.
           3. generate: the C++ of mgp_main, of the Konami modules and of every
              stage, with the versioned analysis hints (no Ghidra needed).
              A corpus is generated again only when its hints or psp_recomp
              change.
           4. main: configures the build with the generated code and compiles
              mpo_boot (mgp_main and the modules: the long wait, ~20 min on
              4 cores). After this the game can be played.
  stages   compiles the stage overlays one by one, in the order of
           profiles/mpo/config/build_order.toml, and moves each finished
           library into <data>/overlays, where a running mpo_boot finds it the
           next time the game loads that stage (until then the stage runs on
           the interpreter). With --low-priority it yields the CPU to the game.
           The objects of an installed stage are deleted (the C++ stays).
  play     runs the game: mpo_boot in a window with <data>/overlays, the
           firmware fonts and the default save folder.
  status   what is done, as text or --json (for the launcher).

Usage:

  python3 -I profiles/mpo/scripts/setup.py install --iso <ULUS-10202.iso> [--data <dir>]
      [--jobs N] [--generated-jobs N] [--fonts <dir>] [--cmake cmake] [--generator Ninja]
      [--cc clang --cxx clang++]
  python3 -I profiles/mpo/scripts/setup.py stages [--data <dir>] [--jobs N] [--low-priority]
      [--only st02,st03t1] [--keep-objects]
  python3 -I profiles/mpo/scripts/setup.py play [--data <dir>] [-- <mpo_boot options>...]
  python3 -I profiles/mpo/scripts/setup.py status [--data <dir>] [--json]

<data> defaults to %LOCALAPPDATA%\\mpo-pc-port\\install on Windows and
$XDG_DATA_HOME/mpo-pc-port/install (~/.local/share/...) elsewhere. It holds the
decrypted game, the generated C++ and the build: nothing in it may be shared.
"""

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
import tomllib

_ROOT = pathlib.Path(__file__).resolve().parents[3]
_PROFILE = _ROOT / 'profiles' / 'mpo'
_MODULES = ('sound2', 'kjfs', 'zlibdec')
_STATE_VERSION = 1
_LIBRARY_SUFFIXES = ('.so', '.dll', '.dylib')
# Where the firmware fonts may already be on the player's machine, besides
# --fonts and flash0/font next to the ISO: PPSSPP keeps a copy in its assets.
_PPSSPP_FONT_DIRS = (
    '/usr/share/ppsspp/assets/flash0/font',
    '/usr/local/share/ppsspp/assets/flash0/font',
    '/var/lib/flatpak/app/org.ppsspp.PPSSPP/current/active/files/share/ppsspp/assets/flash0/font',
    '~/.local/share/flatpak/app/org.ppsspp.PPSSPP/current/active/files/share/ppsspp/assets/flash0/font',
    '~/.local/share/ppsspp/assets/flash0/font',
)
_FONTS = ('ltn0.pgf', 'jpn0.pgf')


class SetupError(Exception):
  """A step failed; the message says which and where its log is."""


def _sha256_bytes(data):
  return hashlib.sha256(data).hexdigest()


def _sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def _load_script(name):
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def default_data_dir(environ=None, system=None):
  """Returns the default <data> folder for this OS."""
  environ = os.environ if environ is None else environ
  system = platform.system() if system is None else system
  if system == 'Windows':
    base = environ.get('LOCALAPPDATA') or str(pathlib.Path.home() / 'AppData' / 'Local')
    return pathlib.Path(base) / 'mpo-pc-port' / 'install'
  base = environ.get('XDG_DATA_HOME') or str(pathlib.Path.home() / '.local' / 'share')
  return pathlib.Path(base) / 'mpo-pc-port' / 'install'


def natural_key(name):
  """st2 < st10: digit runs compare as numbers."""
  return [int(part) if part.isdigit() else part for part in re.split(r'(\d+)', name)]


def stage_order(stages, order_path):
  """Sorts the stage corpora for compilation (build_order.toml)."""
  with open(order_path, 'rb') as f:
    order = tomllib.load(f)
  first = [s for s in order.get('first', []) if s in stages]
  last_prefixes = tuple(order.get('last_prefixes', []))
  rest = sorted((s for s in stages if s not in first), key=natural_key)
  middle = [s for s in rest if not s.startswith(last_prefixes)]
  last = [s for s in rest if s.startswith(last_prefixes)]
  return first + middle + last


def runtime_key(compiler_id, opt_level):
  """Identifies what a stage library is compiled against.

  The generated code only includes the framework's runtime headers and the
  overlay glue; a change there (or of the compiler or the optimization level)
  means every stage library has to be compiled again.
  """
  digest = hashlib.sha256()
  digest.update(f'{compiler_id}\n{opt_level}\n'.encode())
  headers = sorted((_ROOT / 'framework' / 'psprecomp' / 'include').rglob('*.hpp'))
  glue = [_PROFILE / 'host' / 'hle' / 'overlay_module.hpp',
          _PROFILE / 'host' / 'hle' / 'overlay_module.cpp.in']
  for path in headers + glue:
    digest.update(path.relative_to(_ROOT).as_posix().encode() + b'\n')
    digest.update(path.read_bytes())
  return digest.hexdigest()


class Setup:
  """The steps, over one <data> folder."""

  def __init__(self, args):
    self.args = args
    self.data = pathlib.Path(args.data).resolve() if args.data else default_data_dir()
    self.game = self.data / 'game'
    self.generated = self.data / 'generated'
    self.build = self.data / 'build'
    self.overlays = self.data / 'overlays'
    self.logs = self.data / 'logs'
    self.state_path = self.data / 'state.json'
    self.state = self._load_state()

  # -- state -----------------------------------------------------------------

  def _load_state(self):
    if self.state_path.is_file():
      try:
        state = json.loads(self.state_path.read_text(encoding='utf-8'))
        if state.get('version') == _STATE_VERSION:
          return state
      except (OSError, ValueError):
        pass
    return {'version': _STATE_VERSION, 'stages': {}}

  def _save_state(self):
    self.data.mkdir(parents=True, exist_ok=True)
    tmp = self.state_path.with_suffix('.tmp')
    tmp.write_text(json.dumps(self.state, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(tmp, self.state_path)

  # -- helpers ---------------------------------------------------------------

  def _note(self, text):
    print(f'setup: {text}', flush=True)

  def _run(self, command, log_name, low_priority=False, env=None):
    """Runs a command with its output in <data>/logs/<log_name>.log."""
    self.logs.mkdir(parents=True, exist_ok=True)
    log_path = self.logs / f'{log_name}.log'
    kwargs = {}
    if low_priority:
      if os.name == 'nt':
        kwargs['creationflags'] = subprocess.BELOW_NORMAL_PRIORITY_CLASS
      else:
        kwargs['preexec_fn'] = lambda: os.nice(15)
    with open(log_path, 'a', encoding='utf-8', errors='replace') as log:
      log.write(f'\n$ {" ".join(str(c) for c in command)}\n')
      log.flush()
      result = subprocess.run([str(c) for c in command], stdout=log, stderr=subprocess.STDOUT,
                              env=env, check=False, **kwargs)
    if result.returncode != 0:
      tail = log_path.read_text(encoding='utf-8', errors='replace').splitlines()[-25:]
      raise SetupError(f'{command[0]} failed (exit {result.returncode}); log: {log_path}\n' +
                       '\n'.join(tail))
    return log_path

  def _exe(self, relative):
    path = self.build / relative
    if os.name == 'nt':
      path = path.with_suffix('.exe')
    return path

  @property
  def mpo(self):
    return self._exe('profiles/mpo/host/mpo')

  @property
  def psp_recomp(self):
    return self._exe('framework/psprecomp/psp_recomp')

  @property
  def mpo_boot(self):
    return self._exe('profiles/mpo/host/hle/mpo_boot')

  def _cmake_configure(self, extra, log_name):
    args = self.args
    command = [args.cmake, '-S', _ROOT, '-B', self.build, '-G', args.generator,
               '-DCMAKE_BUILD_TYPE=Release', '-DPSPRECOMP_BUILD_TESTS=OFF',
               '-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF']
    if args.cc:
      command.append(f'-DCMAKE_C_COMPILER={args.cc}')
    if args.cxx:
      command.append(f'-DCMAKE_CXX_COMPILER={args.cxx}')
    if args.generated_jobs:
      command.append(f'-DPSPRECOMP_GENERATED_JOBS={args.generated_jobs}')
    command += extra
    self._run(command, log_name)

  def _cmake_build(self, targets, log_name, low_priority=False):
    command = [self.args.cmake, '--build', self.build, '--target', *targets]
    if self.args.jobs:
      command += ['--parallel', str(self.args.jobs)]
    self._run(command, log_name, low_priority=low_priority)

  def _cache_value(self, name):
    cache = self.build / 'CMakeCache.txt'
    if not cache.is_file():
      return None
    for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
      if line.startswith(f'{name}:'):
        return line.split('=', 1)[1]
    return None

  # -- install ---------------------------------------------------------------

  def step_tools(self):
    self._note(f'tools: configuring {self.build}')
    self._cmake_configure([], 'tools')
    self._note('tools: compiling mpo and psp_recomp')
    self._cmake_build(['mpo', 'psp_recomp'], 'tools')

  def step_disc(self, iso):
    iso = pathlib.Path(iso).resolve()
    installed = self.game / 'install.toml'
    if installed.is_file() and self.state.get('iso') == str(iso):
      self._note(f'disc: already installed from {iso}')
      return
    if not iso.is_file():
      raise SetupError(f'no ISO at {iso}')
    self._note(f'disc: validating and decrypting {iso}')
    try:
      self._run([self.mpo, '--install', iso, '--game-dir', self.game], 'disc')
    except SetupError as error:
      raise SetupError('this ISO is not the supported Metal Gear Solid: Portable Ops '
                       '(ULUS-10202, disc 1.02), or it is damaged.\n' + str(error)) from error
    self.state['iso'] = str(iso)
    self._save_state()

  def _corpus_up_to_date(self, generate, name, out):
    manifest_path = out / 'generate.json'
    if not manifest_path.is_file():
      return False
    try:
      manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except ValueError:
      return False
    hints = generate.profile_hints_path(name)
    if not hints.is_file():
      return False
    expected = _sha256_bytes(generate.strip_profile_header(hints.read_bytes()))
    return (manifest.get('inputs', {}).get(f'{name}.hints.txt') == expected and
            manifest.get('tools', {}).get('psp_recomp') == _sha256(self.psp_recomp))

  def _generate_one(self, name, argv, out):
    """Runs generate.py for one corpus in its own process; returns its log."""
    log_dir = self.logs / 'generate'
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f'{name}.log'
    command = [sys.executable, '-I', _PROFILE / 'scripts' / 'generate.py',
               '--psp-recomp', self.psp_recomp, '--game-dir', self.game,
               '--analysis-dir', self.data / 'analysis', '--hints-from', 'profile',
               '--out', out, *argv]
    with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
      result = subprocess.run([str(c) for c in command], stdout=log, stderr=subprocess.STDOUT,
                              check=False)
    if result.returncode != 0:
      tail = log_path.read_text(encoding='utf-8', errors='replace').splitlines()[-10:]
      raise SetupError(f'generating {name} failed; log: {log_path}\n' + '\n'.join(tail))
    return log_path

  def stage_corpora(self, build_overlays, generate):
    supported = _PROFILE / 'config' / 'supported.toml'
    stages = build_overlays.all_stages(supported)
    kept, aliases = build_overlays.distinct_stages(stages, supported,
                                                   _PROFILE / 'config' / 'stages.toml', generate)
    return kept, aliases

  def step_generate(self):
    generate = _load_script('generate')
    build_overlays = _load_script('build_overlays')
    started = time.monotonic()
    work = [('mgp_main', [], self.generated / 'mgp_main')]
    work += [(m, ['--module', m], self.generated / 'modules' / m) for m in _MODULES]
    kept, _ = self.stage_corpora(build_overlays, generate)
    work += [(f'stage_{s}', ['--stage', s], self.generated / 'overlays' / s) for s in kept]
    pending = [w for w in work if not self._corpus_up_to_date(generate, w[0], w[2])]
    # One psp_recomp per core takes a few hundred MB each; a handful at a time
    # turns ~8 minutes of generation into ~2.
    workers = max(1, min(self.args.generate_jobs or (os.cpu_count() or 2) // 2, 8))
    self._note(f'generate: {len(pending)} corpora to generate ({len(work) - len(pending)} up to '
               f'date), {workers} at a time')
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
      futures = {pool.submit(self._generate_one, *w): w[0] for w in pending}
      for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
        future.result()
        self._note(f'generate: {futures[future]} ({index}/{len(pending)})')
    self._note(f'generate: done in {time.monotonic() - started:.0f} s')

  def step_main(self):
    self._note('main: configuring with the generated code')
    self._cmake_configure([
        f'-DMPO_GENERATED_DIR={self.generated / "mgp_main" / "generated"}',
        f'-DMPO_GENERATED_MODULES_DIR={self.generated / "modules"}',
        f'-DMPO_GENERATED_OVERLAYS_DIR={self.generated / "overlays"}',
    ], 'main')
    self._note('main: compiling mgp_main, the modules and mpo_boot (the long wait)')
    started = time.monotonic()
    self._cmake_build(['mpo_boot'], 'main')
    self._note(f'main: mpo_boot ready in {time.monotonic() - started:.0f} s: the game can be played')
    self.state['main'] = {'mpo_boot': str(self.mpo_boot), 'built': time.time()}
    self._save_state()

  def step_fonts(self):
    found = find_fonts(self.args.fonts, self.state.get('iso'))
    target = self.data / 'flash0' / 'font'
    if found is None:
      if (target / _FONTS[0]).is_file():
        return
      self._note('fonts: no PSP firmware fonts found (flash0/font/ltn0.pgf). The game runs '
                 'without them, but the "Checking Memory Stick" text will not show. Put them in '
                 f'{target} or pass --fonts.')
      return
    if found.resolve() == target.resolve():
      return
    target.mkdir(parents=True, exist_ok=True)
    copied = 0
    for font in sorted(found.glob('*.pgf')):
      shutil.copyfile(font, target / font.name)
      copied += 1
    self._note(f'fonts: {copied} fonts copied from {found}')

  def install(self):
    self.step_tools()
    self.step_disc(self.args.iso)
    self.step_generate()
    self.step_fonts()
    self.step_main()

  # -- stages ----------------------------------------------------------------

  def _library_name(self, stage):
    suffix = '.dll' if os.name == 'nt' else '.dylib' if sys.platform == 'darwin' else '.so'
    return f'stage_{stage}{suffix}'

  def stages(self):
    if not self.mpo_boot.is_file():
      raise SetupError('mpo_boot is not built yet: run `setup.py install` first')
    generate = _load_script('generate')
    build_overlays = _load_script('build_overlays')
    kept, _ = self.stage_corpora(build_overlays, generate)
    only = [s for s in (self.args.only or '').split(',') if s]
    order = stage_order(only or kept, _PROFILE / 'config' / 'build_order.toml')
    key = runtime_key(self._cache_value('CMAKE_CXX_COMPILER') or '',
                      self._cache_value('PSPRECOMP_GENERATED_OPT_LEVEL') or '')
    states = self.state.setdefault('stages', {})
    self.overlays.mkdir(parents=True, exist_ok=True)
    pending = [s for s in order if not self._stage_ready(s, states.get(s), key)]
    self._note(f'stages: {len(order) - len(pending)} ready, {len(pending)} to compile')
    for index, stage in enumerate(pending, 1):
      started = time.monotonic()
      self._note(f'stages: compiling {stage} ({index}/{len(pending)})')
      self._cmake_build([f'mpo_overlay_{stage}'], f'stage_{stage}',
                        low_priority=self.args.low_priority)
      built = self.build / 'overlays' / self._library_name(stage)
      if not built.is_file():
        raise SetupError(f'{stage}: no library at {built}')
      # Atomic: a running mpo_boot never sees half a library.
      tmp = self.overlays / f'.{built.name}.tmp'
      shutil.copyfile(built, tmp)
      os.replace(tmp, self.overlays / built.name)
      states[stage] = {
          'runtime_key': key,
          'generated': _sha256((self.generated / 'overlays' / stage / 'generate.json')),
          'seconds': round(time.monotonic() - started, 1),
      }
      self._save_state()
      if not self.args.keep_objects:
        objects = self.build / 'profiles' / 'mpo' / 'host' / 'hle' / 'CMakeFiles' / \
            f'mpo_overlay_{stage}.dir'
        shutil.rmtree(objects, ignore_errors=True)
      self._note(f'stages: {stage} installed in {states[stage]["seconds"]:.0f} s')
    self._note('stages: all stages compiled')

  def _stage_ready(self, stage, state, key):
    if not state or state.get('runtime_key') != key:
      return False
    manifest = self.generated / 'overlays' / stage / 'generate.json'
    if not manifest.is_file() or state.get('generated') != _sha256(manifest):
      return False
    return (self.overlays / self._library_name(stage)).is_file()

  # -- play ------------------------------------------------------------------

  def play(self, extra):
    if not self.mpo_boot.is_file():
      raise SetupError('mpo_boot is not built yet: run `setup.py install` first')
    iso = self.state.get('iso')
    if not iso or not pathlib.Path(iso).is_file():
      raise SetupError(f'the ISO {iso} is gone: run `setup.py install --iso <new place>`')
    command = [self.mpo_boot, '--elf', self.game / 'elf' / 'mgp_main.elf', '--iso', iso,
               '--window', '1', '--overlays', self.overlays,
               '--renderer', os.environ.get('MPO_RENDERER', 'vulkan')]
    fonts = self.data / 'flash0' / 'font'
    if fonts.is_dir():
      command += ['--fonts', fonts]
    command += extra
    print('setup: ' + ' '.join(str(c) for c in command), flush=True)
    return subprocess.run([str(c) for c in command], check=False).returncode

  # -- status ----------------------------------------------------------------

  def status(self):
    stages = self.state.get('stages', {})
    libraries = sorted(p.name for p in self.overlays.glob('stage_*')
                       if p.suffix in _LIBRARY_SUFFIXES) if self.overlays.is_dir() else []
    return {
        'data': str(self.data),
        'iso': self.state.get('iso'),
        'disc_installed': (self.game / 'install.toml').is_file(),
        'playable': self.mpo_boot.is_file() and 'main' in self.state,
        'stages_installed': len(libraries),
        'stages_compiled_seconds': round(sum(s.get('seconds', 0) for s in stages.values())),
    }


def find_fonts(explicit, iso):
  """Returns a folder with the firmware fonts, or None."""
  candidates = []
  if explicit:
    candidates.append(pathlib.Path(explicit))
  if iso:
    candidates.append(pathlib.Path(iso).parent / 'flash0' / 'font')
  candidates += [pathlib.Path(p).expanduser() for p in _PPSSPP_FONT_DIRS]
  for folder in candidates:
    if all((folder / font).is_file() for font in _FONTS):
      return folder
  if explicit:
    raise SetupError(f'--fonts {explicit}: no {" or ".join(_FONTS)} there')
  return None


def parse_args(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  sub = parser.add_subparsers(dest='command', required=True)

  def common(p):
    p.add_argument('--data', help='the install folder (default: per-user data folder)')

  def build_options(p):
    p.add_argument('--jobs', type=int, help='parallel compile jobs (default: the build tool\'s)')
    p.add_argument('--cmake', default='cmake')

  install = sub.add_parser('install', help='validate the ISO, generate and compile mpo_boot')
  common(install)
  build_options(install)
  install.add_argument('--iso', required=True)
  install.add_argument('--fonts', help='folder with the PSP firmware fonts (flash0/font)')
  install.add_argument('--generator', default='Ninja')
  install.add_argument('--cc')
  install.add_argument('--cxx')
  install.add_argument('--generate-jobs', type=int,
                       help='corpora generated at a time (default: half the cores, at most 8)')
  install.add_argument('--generated-jobs', type=int,
                       help='parallel jobs for the generated code (default: 1 per 4 GiB of RAM)')

  stages = sub.add_parser('stages', help='compile the stage overlays in the background order')
  common(stages)
  build_options(stages)
  stages.add_argument('--low-priority', action='store_true',
                      help='compile at a low CPU priority (while the game runs)')
  stages.add_argument('--only', help='comma-separated stages')
  stages.add_argument('--keep-objects', action='store_true',
                      help='keep the object files of the stages already installed')

  play = sub.add_parser('play', help='run the game')
  common(play)
  play.add_argument('extra', nargs=argparse.REMAINDER,
                    help='options for mpo_boot, after --')

  status = sub.add_parser('status', help='what is done')
  common(status)
  status.add_argument('--json', action='store_true')
  return parser.parse_args(argv)


def main(argv):
  args = parse_args(argv)
  try:
    setup = Setup(args)
    if args.command == 'install':
      setup.install()
    elif args.command == 'stages':
      setup.stages()
    elif args.command == 'play':
      extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
      return setup.play(extra)
    elif args.command == 'status':
      status = setup.status()
      if args.json:
        print(json.dumps(status, indent=2))
      else:
        for key, value in status.items():
          print(f'{key}: {value}')
  except (OSError, ValueError, SetupError) as error:
    print(f'setup: {error}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
