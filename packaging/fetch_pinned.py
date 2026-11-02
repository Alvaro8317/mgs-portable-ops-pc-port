#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads one file pinned in profiles/mpo/config/toolchain.toml (F22b, release workflow).

Checks its SHA-256 (and size) before anything else and, with --extract,
unpacks it (.zip or .tar.xz/.tar.gz) refusing absolute paths and `..`. Prints
the path of the file, or with --extract the folder named `top`.

  python3 -I packaging/fetch_pinned.py windows-x86_64 python --out dl/
  python3 -I packaging/fetch_pinned.py release-linux-x86_64 llvm-mingw --out tools/ --extract
"""

import argparse
import hashlib
import pathlib
import sys
import tarfile
import tomllib
import urllib.request
import zipfile

_ROOT = pathlib.Path(__file__).resolve().parents[1]


def _safe(names):
  for name in names:
    parts = pathlib.PurePosixPath(name.replace('\\', '/')).parts
    if name.startswith(('/', '\\')) or (parts and ':' in parts[0]) or '..' in parts:
      raise SystemExit(f'fetch_pinned: unsafe path in the archive: {name!r}')


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('table')
  parser.add_argument('tool')
  parser.add_argument('--out', required=True)
  parser.add_argument('--extract', action='store_true')
  args = parser.parse_args(argv)
  with open(_ROOT / 'profiles' / 'mpo' / 'config' / 'toolchain.toml', 'rb') as f:
    spec = tomllib.load(f)[args.table][args.tool]
  out = pathlib.Path(args.out)
  out.mkdir(parents=True, exist_ok=True)
  target = out / pathlib.PurePosixPath(spec['url']).name
  digest = hashlib.sha256()
  request = urllib.request.Request(spec['url'], headers={'User-Agent': 'mpo-pc-port-release'})
  with urllib.request.urlopen(request, timeout=120) as response, open(target, 'wb') as f:
    for block in iter(lambda: response.read(1 << 20), b''):
      f.write(block)
      digest.update(block)
  if digest.hexdigest() != spec['sha256']:
    target.unlink()
    raise SystemExit(f'fetch_pinned: {spec["url"]}: SHA-256 {digest.hexdigest()}, pinned '
                     f'{spec["sha256"]}')
  if 'size' in spec and target.stat().st_size != spec['size']:
    target.unlink()
    raise SystemExit(f'fetch_pinned: {spec["url"]}: wrong size')
  if not args.extract:
    print(target)
    return 0
  if target.suffix == '.zip':
    with zipfile.ZipFile(target) as z:
      _safe(z.namelist())
      z.extractall(out)
  else:
    with tarfile.open(target) as t:
      _safe(t.getnames())
      t.extractall(out, filter='data')
  target.unlink()
  print(out / spec['top'] if spec['top'] else out)
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
