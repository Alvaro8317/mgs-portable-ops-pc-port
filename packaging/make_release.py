#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the release archives and checks that they carry nothing of the game (F22b).

The release is the port's own code, the launcher and the tools to compile the
game on the player's machine; never anything of the game, translated or not
(CLAUDE.md §1, docs/analysis/distribution.md §1).

  source   <out.tar.gz>   the tracked files of the repo and of the framework
                          submodule, without the submodule's profiles/ (the
                          upstream sample profile carries generated code and
                          binaries of another game).
  windows  <out.zip>      mpo_launcher.exe, the embeddable Python, the source
                          and the prebuilt FFmpeg (with its source tarball):
                          --launcher, --python-zip, --ffmpeg, --ffmpeg-source.
  check    <archive>...   fails if an archive holds a game file: by path
                          (game/, analysis/, generated/, ISO, EBOOT, PRX...)
                          and by content (~PSP, MIPS ELF, ISO 9660, PBP, PSF).

  python3 -I packaging/make_release.py source dist/mpo-pc-port-0.1-source.tar.gz
  python3 -I packaging/make_release.py windows dist/mpo-pc-port-0.1-windows-x86_64.zip \\
      --launcher mpo_launcher.exe --python-zip python-3.14.10-embed-amd64.zip \\
      --ffmpeg ffmpeg-prebuilt --ffmpeg-source ffmpeg-7.1.5.tar.xz
  python3 -I packaging/make_release.py check dist/*.zip dist/*.tar.gz
"""

import argparse
import gzip
import hashlib
import io
import pathlib
import re
import subprocess
import sys
import tarfile
import tomllib
import zipfile

_ROOT = pathlib.Path(__file__).resolve().parents[1]
_FRAMEWORK = 'framework/psprecomp'
# Directories of the submodule that never ship.
_FRAMEWORK_EXCLUDED = ('profiles/',)
# Path components that only hold the player's or the developer's game data.
_FORBIDDEN_DIRS = {'game', 'analysis', 'generated', 'progress', 'captures', 'dumps', 'traces'}
# docs/analysis is prose and numbers only (F03), versioned on purpose.
_ALLOWED_DIR_PATHS = ('docs/analysis/',)
_FORBIDDEN_SUFFIXES = ('.iso', '.cso', '.chd', '.pbp', '.prx', '.rlc', '.elf', '.pgf', '.ppdmp',
                       '.gzf', '.gpr', '.input', '.sfo', '.dar', '.qar', '.txp', '.mdp', '.mtar',
                       '.pmf', '.at3', '.gcx')
_FORBIDDEN_NAMES = {'eboot.bin', 'boot.bin', 'param.sfo', 'slot.dat', '_zar', 'data.cnf'}
_SPDX_RE = re.compile(rb'SPDX-License-Identifier')


class ReleaseError(Exception):
  """The archive cannot be built, or it holds something it must not."""


def _git_files(repo, prefix=''):
  out = subprocess.run(['git', '-C', str(repo), 'ls-files', '-z', '--stage'], check=True,
                       capture_output=True).stdout
  files = []
  for entry in out.split(b'\0'):
    if not entry:
      continue
    meta, path = entry.split(b'\t', 1)
    if meta.startswith(b'160000'):
      continue  # a submodule: listed on its own
    files.append(prefix + path.decode('utf-8'))
  return files


def source_files(root=_ROOT):
  """The relative paths of the source release, sorted."""
  files = _git_files(root)
  framework = root / _FRAMEWORK
  if not (framework / '.git').exists():
    raise ReleaseError(f'{_FRAMEWORK} is not checked out (git submodule update --init)')
  for path in _git_files(framework, _FRAMEWORK + '/'):
    inner = path[len(_FRAMEWORK) + 1:]
    if not inner.startswith(_FRAMEWORK_EXCLUDED):
      files.append(path)
  return sorted(files)


def forbidden_path(path):
  """Why `path` (inside an archive) must not ship, or None."""
  normalized = path.replace('\\', '/').lstrip('/')
  lowered = normalized.lower()
  parts = [p for p in lowered.split('/') if p]
  if not parts:
    return None
  for index, part in enumerate(parts[:-1]):
    if part in _FORBIDDEN_DIRS:
      prefix = '/'.join(parts[:index + 1]) + '/'
      if not any(prefix.endswith(allowed) for allowed in _ALLOWED_DIR_PATHS):
        return f'a {part}/ folder'
  name = parts[-1]
  if name in _FORBIDDEN_NAMES:
    return f'a game file name ({name})'
  if name.endswith(_FORBIDDEN_SUFFIXES):
    return f'a game file type ({pathlib.PurePosixPath(name).suffix})'
  return None


def forbidden_content(head, data_at_32k=b''):
  """Why a file starting with `head` must not ship, or None."""
  if head.startswith(b'~PSP'):
    return 'an encrypted PSP module (~PSP)'
  if head.startswith(b'\0PBP'):
    return 'a PSP EBOOT (PBP)'
  if head.startswith(b'\0PSF'):
    return 'a PARAM.SFO (PSF)'
  if head.startswith(b'\x7fELF') and len(head) >= 20:
    # e_machine at 18, little-endian (ELFDATA2LSB) or big.
    little = head[5] == 1
    machine = int.from_bytes(head[18:20], 'little' if little else 'big')
    if machine == 8:
      return 'a MIPS ELF (a PSP executable or module)'
  if data_at_32k[1:6] == b'CD001':
    return 'an ISO 9660 image'
  return None


def _check_member(name, read, problems):
  reason = forbidden_path(name)
  if reason:
    problems.append(f'{name}: {reason}')
    return
  head = read(0x8010)
  reason = forbidden_content(head[:64], head[0x8000:0x8010])
  if reason:
    problems.append(f'{name}: {reason}')


def check_archive(path):
  """Returns the problems found in a .zip or .tar.gz (empty: it may ship)."""
  path = pathlib.Path(path)
  problems = []
  if path.suffix == '.zip':
    with zipfile.ZipFile(path) as z:
      for info in z.infolist():
        if info.is_dir():
          continue
        def read(n, info=info):
          with z.open(info) as f:
            return f.read(n)
        _check_member(info.filename, read, problems)
        # Archives inside the archive (the embeddable Python's stdlib zip).
        if info.filename.lower().endswith('.zip'):
          inner = io.BytesIO(z.read(info))
          with zipfile.ZipFile(inner) as nested:
            for inner_info in nested.infolist():
              if inner_info.is_dir():
                continue
              def read_inner(n, inner_info=inner_info):
                with nested.open(inner_info) as f:
                  return f.read(n)
              _check_member(f'{info.filename}!{inner_info.filename}', read_inner, problems)
  elif path.name.endswith(('.tar.gz', '.tgz', '.tar')):
    with tarfile.open(path) as t:
      for member in t.getmembers():
        if not member.isfile():
          continue
        def read(n, member=member):
          f = t.extractfile(member)
          return f.read(n) if f else b''
        _check_member(member.name, read, problems)
  else:
    raise ReleaseError(f'{path}: not a .zip or .tar.gz')
  return problems


def _sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def _add_tree(add, root, prefix):
  for path in sorted(pathlib.Path(root).rglob('*')):
    if path.is_file():
      add(path, f'{prefix}/{path.relative_to(root).as_posix()}')


def make_source(out, root=_ROOT, top='mpo-pc-port-source'):
  files = source_files(root)
  out = pathlib.Path(out)
  out.parent.mkdir(parents=True, exist_ok=True)
  # Reproducible: fixed order, times and owners.
  with open(out, 'wb') as raw:
    with gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as gz:
      with tarfile.open(fileobj=gz, mode='w', format=tarfile.PAX_FORMAT) as tar:
        for rel in files:
          src = root / rel
          if src.is_symlink() or not src.is_file():
            continue
          info = tar.gettarinfo(str(src), arcname=f'{top}/{rel}')
          info.uid = info.gid = 0
          info.uname = info.gname = ''
          info.mtime = 0
          with open(src, 'rb') as f:
            tar.addfile(info, f)
  return len(files)


def _python_pin(root=_ROOT):
  with open(root / 'profiles' / 'mpo' / 'config' / 'toolchain.toml', 'rb') as f:
    return tomllib.load(f)['windows-x86_64']['python']


def make_windows(out, launcher, python_zip, ffmpeg, ffmpeg_source, root=_ROOT,
                 top='mpo-pc-port'):
  pin = _python_pin(root)
  if _sha256(python_zip) != pin['sha256']:
    raise ReleaseError(f'{python_zip} is not the pinned Python {pin["version"]} '
                       '(profiles/mpo/config/toolchain.toml)')
  ffmpeg = pathlib.Path(ffmpeg)
  if not (ffmpeg / 'include' / 'libavcodec' / 'avcodec.h').is_file():
    raise ReleaseError(f'{ffmpeg} is not a prebuilt FFmpeg (packaging/ffmpeg)')
  out = pathlib.Path(out)
  out.parent.mkdir(parents=True, exist_ok=True)
  with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    def add(src, name):
      z.write(src, f'{top}/{name}')
    add(launcher, 'mpo_launcher.exe')
    add(root / 'LICENSE', 'LICENSE.txt')
    add(root / 'packaging' / 'windows' / 'README.txt', 'README.txt')
    with zipfile.ZipFile(python_zip) as pz:
      for info in pz.infolist():
        if not info.is_dir():
          z.writestr(f'{top}/python/{info.filename}', pz.read(info))
    _add_tree(add, ffmpeg, 'ffmpeg')
    add(ffmpeg_source, f'ffmpeg/{pathlib.Path(ffmpeg_source).name}')
    for rel in source_files(root):
      src = root / rel
      if src.is_file() and not src.is_symlink():
        add(src, f'source/{rel}')


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  sub = parser.add_subparsers(dest='command', required=True)
  source = sub.add_parser('source')
  source.add_argument('out')
  windows = sub.add_parser('windows')
  windows.add_argument('out')
  windows.add_argument('--launcher', required=True)
  windows.add_argument('--python-zip', required=True)
  windows.add_argument('--ffmpeg', required=True)
  windows.add_argument('--ffmpeg-source', required=True)
  check = sub.add_parser('check')
  check.add_argument('archives', nargs='+')
  args = parser.parse_args(argv)
  try:
    if args.command == 'source':
      count = make_source(args.out)
      print(f'make_release: {args.out} ({count} files)')
    elif args.command == 'windows':
      make_windows(args.out, args.launcher, args.python_zip, args.ffmpeg, args.ffmpeg_source)
      print(f'make_release: {args.out}')
    if args.command in ('source', 'windows'):
      archives = [args.out]
    else:
      archives = args.archives
    failed = False
    for archive in archives:
      problems = check_archive(archive)
      for problem in problems:
        print(f'make_release: {archive}: {problem}', file=sys.stderr)
      failed = failed or bool(problems)
      print(f'make_release: {archive}: {"FAILED" if problems else "nothing of the game"}')
    return 1 if failed else 0
  except (OSError, ReleaseError, subprocess.CalledProcessError, zipfile.BadZipFile,
          tarfile.TarError) as error:
    print(f'make_release: {error}', file=sys.stderr)
    return 2


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
