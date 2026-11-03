#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the port on the player's machine, from their ISO to a playable game (F22).

The port never ships the game's code, translated or not (CLAUDE.md §1): this
script turns the player's own ISO into the native game, on their machine, and
is what the launcher runs. Every step is resumable; running it again only does
what is missing.

  toolchain  (Windows) downloads the portable compiler (llvm-mingw), CMake and
           Ninja pinned in profiles/mpo/config/toolchain.toml into
           <data>/toolchain, checking each SHA-256. `install` runs it first
           on Windows; elsewhere the system's (or the Flatpak SDK's) are used.
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
           the interpreter). Two passes (F22b): first every stage at -O0
           (`quick`, ~14x faster to compile), then at the optimized level,
           each one replacing its quick library. With --low-priority it
           yields the CPU to the game. The objects of an installed stage are
           deleted (the C++ stays).
           <data>/stop-stages makes it stop after the stage it is on (the
           launcher's Stop button).
  play     runs the game: mpo_boot in a window with <data>/overlays, the
           firmware fonts and the default save folder.
  import-save <dir> / export-save <dir>
           `mpo --import-save/--export-save` (F18c) with the installed disc:
           a PSP or PPSSPP save into the port's save folder, and back.
  status   what is done, as text or --json (for the launcher).

Usage:

  python3 -I profiles/mpo/scripts/setup.py toolchain [--data <dir>] [--json]
  python3 -I profiles/mpo/scripts/setup.py install --iso <ULUS-10202.iso> [--data <dir>]
      [--jobs N] [--generated-jobs N] [--fonts <dir>] [--cmake cmake] [--generator Ninja]
      [--cc clang --cxx clang++] [--toolchain auto|portable|system] [--ffmpeg-root <dir>]
  python3 -I profiles/mpo/scripts/setup.py stages [--data <dir>] [--jobs N] [--low-priority]
      [--only st02,st03t1] [--keep-objects] [--level both|quick|optimized]
  python3 -I profiles/mpo/scripts/setup.py play [--data <dir>] [-- <mpo_boot options>...]
  python3 -I profiles/mpo/scripts/setup.py import-save|export-save <dir> [--data <dir>]
      [--overwrite]
  python3 -I profiles/mpo/scripts/setup.py status [--data <dir>] [--json]

<data> defaults to %LOCALAPPDATA%\\mpo-pc-port\\install on Windows and
$XDG_DATA_HOME/mpo-pc-port/install (~/.local/share/...) elsewhere. It holds the
decrypted game, the generated C++ and the build: nothing in it may be shared.
"""

import argparse
import concurrent.futures
import contextlib
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import tomllib
import urllib.request
import zipfile

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
# The optimization level of the first, fast pass over the stages (F22b): a
# mission unit compiles in 1.5 s at -O0 against 20.8 s at -O2 (F21).
QUICK_LEVEL = '0'
# The release layout next to the source tree (packaging/windows): the FFmpeg
# the CI built with the pinned llvm-mingw.
_RELEASE_FFMPEG = _ROOT.parent / 'ffmpeg'


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


# Windows reports a process that died as an NTSTATUS exit code; these are the
# ones a player can act on.
_WINDOWS_EXIT_CODES = {
    0xC0000005: 'it crashed (access violation)',
    0xC000001D: 'it crashed (illegal instruction)',
    0xC00000FD: 'it crashed (stack overflow)',
    0xC0000409: 'it crashed (fail fast)',
    0xC000007B: 'a DLL is damaged or for another architecture',
    0xC0000135: 'a DLL it needs is missing',
    0xC0000139: 'a DLL it needs is of another version',
    0xC0000142: 'a DLL it needs failed to start',
}


def describe_exit(code, system=None):
  """Says how a process ended, from its subprocess return code."""
  system = platform.system() if system is None else system
  if code < 0:
    try:
      name = signal.Signals(-code).name
    except ValueError:
      name = f'signal {-code}'
    return f'killed by {name}'
  if system == 'Windows' and code > 0xFFFF:
    meaning = _WINDOWS_EXIT_CODES.get(code & 0xFFFFFFFF)
    text = f'exit code 0x{code & 0xFFFFFFFF:08X}'
    return f'{text}: {meaning}' if meaning else text
  return f'exit code {code}'


def missing_generated_files(out):
  """The files a corpus's generate.json lists that are not on disk.

  None when there is no readable manifest. A file that vanished after
  generating (an antivirus, F22c) would otherwise drop out of CMake's glob and
  fail the link with undefined `register_*_unit_N` symbols.
  """
  try:
    manifest = json.loads((pathlib.Path(out) / 'generate.json').read_text(encoding='utf-8'))
  except (OSError, ValueError):
    return None
  names = manifest.get('generated', {})
  return sorted(n for n in names if not (pathlib.Path(out) / 'generated' / n).is_file())


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


def toolchain_platform(system=None, machine=None):
  """The toolchain.toml table for this machine, or None (use the system's)."""
  system = platform.system() if system is None else system
  machine = (platform.machine() if machine is None else machine).lower()
  if system == 'Windows' and machine in ('amd64', 'x86_64'):
    return 'windows-x86_64'
  return None


def load_toolchain(path, key):
  """The tools to download for `key`, by name (the bundled ones excluded)."""
  with open(path, 'rb') as f:
    table = tomllib.load(f).get(key, {})
  tools = {}
  for name, spec in table.items():
    for field in ('version', 'url', 'sha256', 'top', 'bin'):
      if field not in spec:
        raise SetupError(f'{path}: [{key}.{name}] has no {field}')
    if not re.fullmatch(r'[0-9a-f]{64}', spec['sha256']):
      raise SetupError(f'{path}: [{key}.{name}] sha256 is not a SHA-256')
    if not spec.get('bundled'):
      tools[name] = spec
  return tools


def check_archive_members(names):
  """Rejects absolute paths and `..` in an archive before unpacking it."""
  for name in names:
    parts = pathlib.PurePosixPath(name.replace('\\', '/')).parts
    if name.startswith(('/', '\\')) or (parts and ':' in parts[0]) or '..' in parts:
      raise SetupError(f'unsafe path in the archive: {name!r}')


def cmake_cache_source(cache):
  """The source folder a CMakeCache.txt was made from, or None."""
  try:
    with open(cache, encoding='utf-8', errors='replace') as f:
      for line in f:
        if line.startswith('CMAKE_HOME_DIRECTORY:'):
          return pathlib.Path(line.split('=', 1)[1].strip())
  except OSError:
    pass
  return None


def same_path(a, b):
  """Whether two paths name the same folder (case-insensitive on Windows)."""
  def norm(p):
    return os.path.normcase(os.path.normpath(os.path.abspath(str(p))))
  return norm(a) == norm(b)


# The list of the files a toolchain archive unpacked (F22c), next to its
# .mpo-sha256 stamp: an antivirus can quarantine one of them after unpacking
# (clang-23.exe, the real compiler behind llvm-mingw's 13 KB clang.exe, on a
# player's Windows), and the stamp alone would call the folder complete.
_TOOLCHAIN_FILES = '.mpo-files'


def missing_toolchain_files(folder):
  """The files of an unpacked toolchain folder that are gone.

  Returns None when the folder has no list of its files (unpacked before
  F22c): it cannot be checked, so it is unpacked again.
  """
  listing = pathlib.Path(folder) / _TOOLCHAIN_FILES
  try:
    names = listing.read_text(encoding='utf-8').splitlines()
  except OSError:
    return None
  return [n for n in names if n and not (pathlib.Path(folder) / n).is_file()]


def antivirus_hint(folder, data):
  # Avast quarantined clang-23.exe on a player's Windows (F22c).
  return (f'An antivirus may have removed or blocked it (Avast does with clang-23.exe). It is a '
          f'false positive: the file is the official llvm-mingw release, checked against its '
          f'SHA-256. Restore it (Avast: Protection > Virus Chest; Windows Security: Protection '
          f'history), add an exception for the folder {data}, and press Install again. '
          f'({folder})')


def library_name(stage, quick=False, system=None):
  """The file of a stage library in <data>/overlays.

  The optimized library is stage_<s>.<ext>; the quick one (-O0) has its own
  name, so a running game that has it mapped (Windows cannot replace a loaded
  DLL) never blocks installing the optimized one next to it.
  """
  system = platform.system() if system is None else system
  suffix = '.dll' if system == 'Windows' else '.dylib' if system == 'Darwin' else '.so'
  return f'stage_{stage}-O{QUICK_LEVEL}{suffix}' if quick else f'stage_{stage}{suffix}'


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
    self.toolchain_dir = self.data / 'toolchain'
    self.stop_file = self.data / 'stop-stages'
    self.state_path = self.data / 'state.json'
    self.state = self._load_state()
    self._use_toolchain_path()

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

  def _use_toolchain_path(self):
    # llvm-mingw links libc++ as a DLL: psp_recomp, mpo and mpo_boot find it,
    # and CMake finds the tools, through PATH.
    toolchain = self.state.get('toolchain')
    if not toolchain:
      return
    dirs = [d for d in toolchain.get('path', []) if d not in os.environ.get('PATH', '')]
    if dirs:
      os.environ['PATH'] = os.pathsep.join(dirs + [os.environ.get('PATH', '')])

  @contextlib.contextmanager
  def lock(self):
    """One install or stage compile per <data> at a time.

    An OS file lock, so it goes away with the process however it ends.
    """
    self.data.mkdir(parents=True, exist_ok=True)
    handle = open(self.data / 'setup.lock', 'a+b')
    try:
      try:
        if os.name == 'nt':
          import msvcrt  # pylint: disable=import-outside-toplevel
          handle.seek(0)
          msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
          import fcntl  # pylint: disable=import-outside-toplevel
          fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
      except OSError as error:
        raise SetupError(f'another install or stage compile is running in {self.data}; wait for '
                         'it to end (or end it) and try again') from error
      yield
    finally:
      handle.close()

  # -- helpers ---------------------------------------------------------------

  def _note(self, text):
    # With --json, stdout is only the JSON.
    stream = sys.stderr if getattr(self.args, 'json', False) else sys.stdout
    print(f'setup: {text}', file=stream, flush=True)

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
      start = log.tell()
      result = subprocess.run([str(c) for c in command], stdout=log, stderr=subprocess.STDOUT,
                              env=env, check=False, **kwargs)
    if result.returncode != 0:
      # Only this run's lines: the log keeps every run, and an old error in the
      # tail looked like the new one (F22c, on the player's Windows).
      with open(log_path, 'rb') as log:
        log.seek(start)
        output = log.read().decode('utf-8', errors='replace')
      tail = output.splitlines()[-25:]
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

  @property
  def cmake(self):
    explicit = getattr(self.args, 'cmake', None)
    return explicit or self.state.get('toolchain', {}).get('cmake') or 'cmake'

  def _forget_moved_source(self):
    """Starts the CMake cache again when the source tree is somewhere else.

    A new release unpacked in another folder reuses <data>/build, and CMake
    refuses a cache made from another source ("does not match the source used
    to generate cache", F22c). The objects stay: Ninja rebuilds what changed.
    """
    cache = self.build / 'CMakeCache.txt'
    old = cmake_cache_source(cache)
    if old is None or same_path(old, _ROOT):
      return
    self._note(f'build: the source moved from {old} to {_ROOT}: configuring the build again')
    cache.unlink(missing_ok=True)
    shutil.rmtree(self.build / 'CMakeFiles', ignore_errors=True)

  def _cmake_configure(self, extra, log_name):
    self._forget_moved_source()
    args = self.args
    toolchain = self.state.get('toolchain', {})
    command = [self.cmake, '-S', _ROOT, '-B', self.build, '-G', args.generator,
               '-DCMAKE_BUILD_TYPE=Release', '-DPSPRECOMP_BUILD_TESTS=OFF',
               '-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF', '-DMPO_BUILD_LAUNCHER=OFF']
    cc = args.cc or toolchain.get('cc')
    cxx = args.cxx or toolchain.get('cxx')
    if cc:
      command.append(f'-DCMAKE_C_COMPILER={cc}')
    if cxx:
      command.append(f'-DCMAKE_CXX_COMPILER={cxx}')
    if toolchain.get('ninja') and args.generator == 'Ninja':
      command.append(f'-DCMAKE_MAKE_PROGRAM={toolchain["ninja"]}')
    ffmpeg = self._ffmpeg_root()
    if ffmpeg:
      command += ['-DMPO_FFMPEG=prebuilt', f'-DMPO_FFMPEG_ROOT={ffmpeg}']
    if args.generated_jobs:
      command.append(f'-DPSPRECOMP_GENERATED_JOBS={args.generated_jobs}')
    command += extra
    self._run(command, log_name)

  def _ffmpeg_root(self):
    explicit = getattr(self.args, 'ffmpeg_root', None)
    if explicit:
      return pathlib.Path(explicit).resolve()
    # Only where the build cannot compile FFmpeg itself (no POSIX shell).
    if os.name == 'nt' and (_RELEASE_FFMPEG / 'include' / 'libavcodec').is_dir():
      return _RELEASE_FFMPEG
    return None

  def _cmake_build(self, targets, log_name, low_priority=False):
    command = [self.cmake, '--build', self.build, '--target', *targets]
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

  # -- toolchain -------------------------------------------------------------

  def _download(self, name, spec, target):
    """Downloads `spec['url']` to `target`, checking its SHA-256."""
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_name(target.name + '.part')
    digest = hashlib.sha256()
    expected_size = spec.get('size')
    done = 0
    shown = -1
    request = urllib.request.Request(spec['url'], headers={'User-Agent': 'mpo-pc-port-setup'})
    with urllib.request.urlopen(request, timeout=60) as response, open(part, 'wb') as out:
      total = expected_size or int(response.headers.get('Content-Length') or 0)
      while True:
        block = response.read(1 << 20)
        if not block:
          break
        out.write(block)
        digest.update(block)
        done += len(block)
        if total:
          percent = min(100, done * 100 // total)
          if percent // 10 != shown:
            shown = percent // 10
            self._note(f'toolchain: {name} {percent}% ({done >> 20} of {total >> 20} MB)')
    if expected_size and done != expected_size:
      part.unlink(missing_ok=True)
      raise SetupError(f'{name}: downloaded {done} bytes, expected {expected_size} '
                       f'({spec["url"]})')
    if digest.hexdigest() != spec['sha256']:
      part.unlink(missing_ok=True)
      raise SetupError(f'{name}: the download does not have the pinned SHA-256 '
                       f'(got {digest.hexdigest()}, expected {spec["sha256"]}; {spec["url"]})')
    os.replace(part, target)

  def _unpack(self, name, archive, folder):
    tmp = folder.with_name(folder.name + '.tmp')
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
      check_archive_members(z.namelist())
      self._note(f'toolchain: unpacking {name}')
      z.extractall(tmp)
      files = [i.filename for i in z.infolist() if not i.is_dir()]
    (tmp / _TOOLCHAIN_FILES).write_text('\n'.join(files) + '\n', encoding='utf-8')
    shutil.rmtree(folder, ignore_errors=True)
    os.replace(tmp, folder)
    missing = missing_toolchain_files(folder)
    if missing:
      raise SetupError(f'toolchain: {len(missing)} files of {name} were gone right after '
                       f'unpacking (the first: {missing[0]}). ' +
                       antivirus_hint(folder, self.data))

  def toolchain(self):
    """Downloads and unpacks the portable toolchain (toolchain.toml)."""
    key = toolchain_platform()
    if key is None:
      raise SetupError('there is no portable toolchain for this system: install CMake, Ninja '
                       'and Clang or GCC (docs/analysis/distribution.md §3)')
    tools = load_toolchain(_PROFILE / 'config' / 'toolchain.toml', key)
    roots = {}
    folders = {}
    for name, spec in tools.items():
      folder = self.toolchain_dir / f'{name}-{spec["version"]}'
      folders[name] = folder
      stamp = folder / '.mpo-sha256'
      complete = stamp.is_file() and stamp.read_text(encoding='utf-8').strip() == spec['sha256']
      if complete:
        missing = missing_toolchain_files(folder)
        if missing:
          self._note(f'toolchain: {len(missing)} files of {name} are missing (the first: '
                     f'{missing[0]}; an antivirus may have removed them): unpacking it again')
          complete = False
        elif missing is None:
          self._note(f'toolchain: {name} has no list of its files: unpacking it again')
          complete = False
      if not complete:
        stamp.unlink(missing_ok=True)
        archive = self.toolchain_dir / 'downloads' / pathlib.PurePosixPath(spec['url']).name
        self._note(f'toolchain: downloading {name} {spec["version"]}')
        self._download(name, spec, archive)
        self._unpack(name, archive, folder)
        archive.unlink(missing_ok=True)
        stamp.write_text(spec['sha256'] + '\n', encoding='utf-8')
      else:
        self._note(f'toolchain: {name} {spec["version"]} already there')
      root = folder / spec['top'] if spec['top'] else folder
      roots[name] = root / spec['bin'] if spec['bin'] else root
    exe = '.exe' if key.startswith('windows') else ''
    for name in ('llvm-mingw', 'cmake', 'ninja'):
      if name not in roots:
        raise SetupError(f'toolchain.toml: [{key}] has no {name}')
    toolchain = {
        'platform': key,
        'cc': str(roots['llvm-mingw'] / f'clang{exe}'),
        'cxx': str(roots['llvm-mingw'] / f'clang++{exe}'),
        'cmake': str(roots['cmake'] / f'cmake{exe}'),
        'ctest': str(roots['cmake'] / f'ctest{exe}'),
        'ninja': str(roots['ninja'] / f'ninja{exe}'),
        'path': [str(roots['llvm-mingw']), str(roots['cmake']), str(roots['ninja'])],
        'versions': {name: spec['version'] for name, spec in tools.items()},
    }
    for field in ('cc', 'cxx', 'cmake', 'ninja'):
      if not pathlib.Path(toolchain[field]).is_file():
        raise SetupError(f'toolchain: no {toolchain[field]} after unpacking')
    self._check_compiler(toolchain['cc'], folders['llvm-mingw'])
    self.state['toolchain'] = toolchain
    self._save_state()
    self._use_toolchain_path()
    self._note('toolchain: ready (' + ', '.join(f'{n} {v}' for n, v in
                                                toolchain['versions'].items()) + ')')
    return toolchain

  def _check_compiler(self, cc, folder):
    """Runs `<cc> --version`: the wrapper clang.exe needs clang-<N>.exe and the LLVM DLLs."""
    try:
      result = subprocess.run([cc, '--version'], capture_output=True, text=True,
                              errors='replace', timeout=120, check=False)
      output = (result.stdout + result.stderr).strip()
      ok = result.returncode == 0
    except OSError as e:
      output, ok = str(e), False
    if not ok:
      # The next Install unpacks it again (the stamp says it is not complete).
      (pathlib.Path(folder) / '.mpo-sha256').unlink(missing_ok=True)
      last = output.splitlines()[-1] if output else '(no output)'
      raise SetupError(f'toolchain: the compiler does not run: {last}. ' +
                       antivirus_hint(folder, self.data))
    self._note(f'toolchain: compiler: {output.splitlines()[0] if output else cc}')

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
            manifest.get('tools', {}).get('psp_recomp') == _sha256(self.psp_recomp) and
            not missing_generated_files(out))

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

  def fonts_installed(self):
    return all((self.data / 'flash0' / 'font' / f).is_file() for f in _FONTS)

  def step_fonts(self, required=False):
    found = find_fonts(self.args.fonts, self.state.get('iso'))
    target = self.data / 'flash0' / 'font'
    if found is None and required and not self.fonts_installed():
      raise SetupError(f'no PSP firmware fonts found ({" and ".join(_FONTS)}): choose the '
                       'flash0/font folder of a PSP, or assets/flash0/font of PPSSPP')
    if found is None:
      if (target / _FONTS[0]).is_file():
        return
      self._note('fonts: no PSP firmware fonts found (flash0/font/ltn0.pgf). The game runs '
                 'without them: the save screens and the on-screen keyboard use a substitute '
                 'font, and the game\'s own "Checking Memory Stick" notice does not show. For '
                 f'the PSP\'s look, put them in {target} or pass --fonts.')
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
    mode = self.args.toolchain
    if mode == 'portable' or (mode == 'auto' and toolchain_platform() is not None and
                              not (self.args.cc or self.args.cxx)):
      self.toolchain()
    self.step_tools()
    self.step_disc(self.args.iso)
    self.step_generate()
    self.step_fonts()
    self.step_main()

  # -- stages ----------------------------------------------------------------

  def _set_overlay_level(self, level):
    # Only the stage libraries follow MPO_OVERLAY_OPT_LEVEL; mpo_boot keeps
    # the level it was built with.
    if self._cache_value('MPO_OVERLAY_OPT_LEVEL') == level:
      return
    self._run([self.cmake, '-S', _ROOT, '-B', self.build, f'-DMPO_OVERLAY_OPT_LEVEL={level}'],
              'stages-configure')

  def _drop_quick(self, stage):
    """Deletes a quick library once the optimized one is there."""
    quick = self.overlays / library_name(stage, quick=True)
    try:
      quick.unlink(missing_ok=True)
    except OSError:
      # A running game has it loaded (Windows); the next run deletes it.
      return False
    self.state.setdefault('stages_quick', {}).pop(stage, None)
    return True

  def stages(self):
    if not self.mpo_boot.is_file():
      raise SetupError('mpo_boot is not built yet: run `setup.py install` first')
    generate = _load_script('generate')
    build_overlays = _load_script('build_overlays')
    kept, _ = self.stage_corpora(build_overlays, generate)
    only = [s for s in (self.args.only or '').split(',') if s]
    order = stage_order(only or kept, _PROFILE / 'config' / 'build_order.toml')
    compiler = self._cache_value('CMAKE_CXX_COMPILER') or ''
    optimized = self._cache_value('PSPRECOMP_GENERATED_OPT_LEVEL') or '2'
    levels = {'quick': [QUICK_LEVEL], 'optimized': [optimized],
              'both': [QUICK_LEVEL, optimized]}[self.args.level]
    levels = list(dict.fromkeys(levels))
    optimized_key = runtime_key(compiler, optimized)
    records = self.state.setdefault('stages', {})
    quick_records = self.state.setdefault('stages_quick', {})
    self.overlays.mkdir(parents=True, exist_ok=True)
    self.stop_file.unlink(missing_ok=True)
    for stage in order:
      if self._stage_ready(stage, records.get(stage), optimized_key):
        self._drop_quick(stage)
    for level in levels:
      quick = level != optimized
      key = runtime_key(compiler, level)
      states = quick_records if quick else records
      pass_name = f'quick (-O{level})' if quick else f'optimized (-O{level})'
      pending = [s for s in order
                 if not self._stage_ready(s, states.get(s), key, quick) and
                 not (quick and self._stage_ready(s, records.get(s), optimized_key))]
      self._note(f'stages: {pass_name}: {len(order) - len(pending)} ready, {len(pending)} to '
                 'compile')
      if not pending:
        continue
      self._set_overlay_level(level)
      for index, stage in enumerate(pending, 1):
        if self.stop_file.exists():
          self._note('stages: stopped (asked to); run it again to go on')
          self.stop_file.unlink(missing_ok=True)
          self._save_state()
          return
        started = time.monotonic()
        self._note(f'stages: compiling {stage} ({index}/{len(pending)}, {pass_name})')
        self._regenerate_if_missing(stage)
        built = self._build_stage(stage)
        # Atomic: a running mpo_boot never sees half a library.
        name = library_name(stage, quick)
        tmp = self.overlays / f'.{name}.tmp'
        shutil.copyfile(built, tmp)
        os.replace(tmp, self.overlays / name)
        states[stage] = {
            'runtime_key': key,
            'opt_level': level,
            'generated': _sha256((self.generated / 'overlays' / stage / 'generate.json')),
            'seconds': round(time.monotonic() - started, 1),
        }
        if not quick:
          self._drop_quick(stage)
        self._save_state()
        if not self.args.keep_objects:
          shutil.rmtree(self._stage_objects(stage), ignore_errors=True)
        self._note(f'stages: {stage} installed in {states[stage]["seconds"]:.0f} s '
                   f'({pass_name})')
    self._save_state()
    self._note('stages: all stages compiled')

  def _stage_objects(self, stage):
    return self.build / 'profiles' / 'mpo' / 'host' / 'hle' / 'CMakeFiles' / \
        f'mpo_overlay_{stage}.dir'

  def _build_stage(self, stage):
    """Compiles one stage library and checks that it loads; returns its path.

    On a player's Windows (F22c) an interrupted compile left objects that
    Ninja took as up to date (the link then missed their symbols), and one
    library crashed the game as soon as it was loaded. Each stage now starts
    from no objects, and `mpo_boot --check-overlay` loads the library before it
    is installed; a library that fails is compiled once more from scratch.
    """
    built = self.build / 'overlays' / library_name(stage)
    for attempt in (1, 2):
      if not self.args.keep_objects or attempt == 2:
        shutil.rmtree(self._stage_objects(stage), ignore_errors=True)
      built.unlink(missing_ok=True)
      self._cmake_build([f'mpo_overlay_{stage}'], f'stage_{stage}',
                        low_priority=self.args.low_priority)
      if not built.is_file():
        raise SetupError(f'{stage}: no library at {built}')
      problem = self._check_library(built)
      if problem is None:
        return built
      self._note(f'stages: {stage}: the library does not load ({problem})' +
                 ('; compiling it again from scratch' if attempt == 1 else ''))
    built.unlink(missing_ok=True)
    raise SetupError(f'{stage}: the compiled library does not load ({problem}). Check that the '
                     f'disk has free space. ' + antivirus_hint(built, self.data))

  def _check_library(self, library):
    """None when mpo_boot loads `library`; else what went wrong."""
    try:
      result = subprocess.run([str(self.mpo_boot), '--check-overlay', str(library)],
                              capture_output=True, text=True, errors='replace', timeout=300,
                              check=False)
    except subprocess.TimeoutExpired:
      return 'mpo_boot --check-overlay did not finish'
    if result.returncode == 0:
      return None
    lines = (result.stdout + result.stderr).strip().splitlines()
    return describe_exit(result.returncode) + (f': {lines[-1]}' if lines else '')

  def _regenerate_if_missing(self, stage):
    """Generates a stage again when files of its C++ are gone from disk."""
    out = self.generated / 'overlays' / stage
    missing = missing_generated_files(out)
    if not missing:
      return
    self._note(f'stages: {stage}: {len(missing)} generated files are missing (e.g. '
               f'{missing[0]}); generating it again. ' + antivirus_hint(out, self.data))
    self._generate_one(f'stage_{stage}', ['--stage', stage], out)
    still = missing_generated_files(out)
    if still:
      raise SetupError(f'{stage}: {len(still)} generated files are still missing after '
                       f'generating it again (e.g. {still[0]}). ' + antivirus_hint(out, self.data))

  def _stage_ready(self, stage, state, key, quick=False):
    if not state or state.get('runtime_key') != key:
      return False
    manifest = self.generated / 'overlays' / stage / 'generate.json'
    if not manifest.is_file() or state.get('generated') != _sha256(manifest):
      return False
    return (self.overlays / library_name(stage, quick)).is_file()

  # -- play ------------------------------------------------------------------

  def play(self, extra):
    if not self.mpo_boot.is_file():
      raise SetupError('mpo_boot is not built yet: run `setup.py install` first')
    iso = self.state.get('iso')
    if not iso or not pathlib.Path(iso).is_file():
      raise SetupError(f'the ISO {iso} is gone: run `setup.py install --iso <new place>`')
    # The folder must exist from the start: mpo_boot rescans it when a stage
    # has no corpus, so libraries that `stages` installs later are picked up.
    self.overlays.mkdir(parents=True, exist_ok=True)
    kept, _ = self.stage_corpora(_load_script('build_overlays'), _load_script('generate'))
    ready = sum(1 for s in kept if (self.overlays / library_name(s)).is_file() or
                (self.overlays / library_name(s, quick=True)).is_file())
    if ready < len(kept):
      self._note(f'play: {ready} of {len(kept)} stages compiled; the rest run on the interpreter '
                 '(slower, FPS drops in heavy scenes). Compile them, also while playing, with: '
                 f'setup.py stages --data "{self.data}" --low-priority')
    command = [self.mpo_boot, '--elf', self.game / 'elf' / 'mgp_main.elf', '--iso', iso,
               '--window', '1', '--overlays', self.overlays,
               '--renderer', os.environ.get('MPO_RENDERER', 'vulkan')]
    fonts = self.data / 'flash0' / 'font'
    if fonts.is_dir():
      command += ['--fonts', fonts]
    command += extra
    print('setup: ' + ' '.join(str(c) for c in command), flush=True)
    code = subprocess.run([str(c) for c in command], check=False).returncode
    if code != 0:
      # A DLL that fails to load stops mpo_boot before it prints anything.
      print(f'setup: mpo_boot ended with {describe_exit(code)}', flush=True)
    return code

  # -- saves -----------------------------------------------------------------

  def transfer_save(self, command, folder):
    """mpo --import-save/--export-save with the installed disc (F18c)."""
    if not self.mpo.is_file() or not (self.game / 'install.toml').is_file():
      raise SetupError('the game is not installed yet: run `setup.py install` first')
    argv = [self.mpo, f'--{command}', pathlib.Path(folder).resolve(), '--game-dir', self.game]
    if self.args.overwrite:
      argv.append('--overwrite')
    print('setup: ' + ' '.join(str(c) for c in argv), flush=True)
    result = subprocess.run([str(c) for c in argv], check=False)
    if result.returncode != 0:
      raise SetupError(f'{command} failed (exit {result.returncode}): see the lines above')
    self._note(f'{command}: done')

  # -- status ----------------------------------------------------------------

  def status(self):
    stages = self.state.get('stages', {})
    quick = self.state.get('stages_quick', {})
    libraries = sorted(p.name for p in self.overlays.glob('stage_*')
                       if p.suffix in _LIBRARY_SUFFIXES) if self.overlays.is_dir() else []
    quick_suffix = f'-O{QUICK_LEVEL}'
    optimized_names = {pathlib.Path(n).stem for n in libraries if not
                       pathlib.Path(n).stem.endswith(quick_suffix)}
    quick_names = {pathlib.Path(n).stem[:-len(quick_suffix)] for n in libraries
                   if pathlib.Path(n).stem.endswith(quick_suffix)}
    try:
      total = len(self.stage_corpora(_load_script('build_overlays'), _load_script('generate'))[0])
    except (OSError, ValueError, SetupError):
      total = None
    return {
        'data': str(self.data),
        'iso': self.state.get('iso'),
        'toolchain': self.state.get('toolchain', {}).get('versions'),
        'disc_installed': (self.game / 'install.toml').is_file(),
        'playable': self.mpo_boot.is_file() and 'main' in self.state,
        'fonts': self.fonts_installed(),
        'stages_total': total,
        # A stage with either library runs compiled; `optimized` is the end state.
        'stages_installed': len(optimized_names | quick_names),
        'stages_optimized': len(optimized_names),
        'stages_quick_only': len(quick_names - optimized_names),
        'stages_compiled_seconds': round(sum(s.get('seconds', 0) for s in stages.values()) +
                                         sum(s.get('seconds', 0) for s in quick.values())),
    }


def ppsspp_windows_font_dirs(environ=None):
  """PPSSPP's assets/flash0/font where its Windows installer puts it."""
  environ = os.environ if environ is None else environ
  roots = [environ.get(k) for k in ('ProgramFiles', 'ProgramW6432', 'ProgramFiles(x86)')]
  if environ.get('LOCALAPPDATA'):
    roots.append(str(pathlib.Path(environ['LOCALAPPDATA']) / 'Programs'))
  folders = []
  for root in dict.fromkeys(r for r in roots if r):
    folders.append(pathlib.Path(root) / 'PPSSPP' / 'assets' / 'flash0' / 'font')
  return folders


def find_fonts(explicit, iso):
  """Returns a folder with the firmware fonts, or None."""
  candidates = []
  if explicit:
    candidates.append(pathlib.Path(explicit))
  if iso:
    candidates.append(pathlib.Path(iso).parent / 'flash0' / 'font')
  candidates += [pathlib.Path(p).expanduser() for p in _PPSSPP_FONT_DIRS]
  candidates += ppsspp_windows_font_dirs()
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
    p.add_argument('--cmake', help='cmake to use (default: the portable toolchain\'s, or PATH)')

  toolchain = sub.add_parser('toolchain', help='download the portable compiler, CMake and Ninja')
  common(toolchain)
  toolchain.add_argument('--json', action='store_true', help='print the tool paths as JSON')

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
  install.add_argument('--toolchain', choices=('auto', 'portable', 'system'), default='auto',
                       help='auto: the portable one on Windows (unless --cc/--cxx), else the '
                       'system\'s')
  install.add_argument('--ffmpeg-root', help='a prebuilt FFmpeg (include/, lib/) for the videos')

  stages = sub.add_parser('stages', help='compile the stage overlays in the background order')
  common(stages)
  build_options(stages)
  stages.add_argument('--low-priority', action='store_true',
                      help='compile at a low CPU priority (while the game runs)')
  stages.add_argument('--only', help='comma-separated stages')
  stages.add_argument('--keep-objects', action='store_true',
                      help='keep the object files of the stages already installed')
  stages.add_argument('--level', choices=('both', 'quick', 'optimized'), default='both',
                      help='both: every stage at -O0 first, then optimized (default)')

  fonts = sub.add_parser('fonts', help='copy the PSP firmware fonts into the install')
  common(fonts)
  fonts.add_argument('--fonts', help='folder with the fonts (default: look for them)')

  play = sub.add_parser('play', help='run the game')
  common(play)
  play.add_argument('extra', nargs=argparse.REMAINDER,
                    help='options for mpo_boot, after --')

  for name, text in (('import-save', 'bring a PSP or PPSSPP save into the port'),
                     ('export-save', 'encrypt the port\'s saves for a PSP or PPSSPP')):
    transfer = sub.add_parser(name, help=text)
    common(transfer)
    transfer.add_argument('folder')
    transfer.add_argument('--overwrite', action='store_true')

  status = sub.add_parser('status', help='what is done')
  common(status)
  status.add_argument('--json', action='store_true')
  return parser.parse_args(argv)


def main(argv):
  args = parse_args(argv)
  try:
    setup = Setup(args)
    if args.command == 'toolchain':
      toolchain = setup.toolchain()
      if args.json:
        print(json.dumps(toolchain, indent=2))
    elif args.command == 'install':
      with setup.lock():
        setup.install()
    elif args.command == 'fonts':
      setup.step_fonts(required=True)
    elif args.command == 'stages':
      with setup.lock():
        setup.stages()
    elif args.command in ('import-save', 'export-save'):
      setup.transfer_save(args.command, args.folder)
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
  except (OSError, ValueError, SetupError, zipfile.BadZipFile) as error:
    print(f'setup: {error}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
