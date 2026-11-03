#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hashes a save folder before and after the manual smoke test (F22c).

CLAUDE.md §7: the smoke test never runs against the player's real save
folder, and the saves are hashed before and after to prove it. This takes a
snapshot of a folder (the SHA-256 of every file under it, by relative path)
and later compares the folder against it. It only reads.

  python3 -I packaging/hash_saves.py snapshot <folder> <snapshot.json>
  python3 -I packaging/hash_saves.py compare <snapshot.json> <folder>

`compare` prints what was added, removed or changed and exits 1 when anything
differs, 0 when the folder is exactly as it was. A folder that does not exist
counts as empty, so a save folder that never existed still compares equal.
On Windows, with the release's Python: python\\python.exe -I
source\\packaging\\hash_saves.py ...
"""

import argparse
import hashlib
import json
import pathlib
import sys

_FORMAT = 'mpo-save-snapshot-1'


def hash_folder(folder):
  """Returns {relative path with '/': sha256 hex} for every file under folder."""
  folder = pathlib.Path(folder)
  hashes = {}
  if not folder.is_dir():
    return hashes
  for path in sorted(folder.rglob('*')):
    if not path.is_file():
      continue
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
      for block in iter(lambda f=f: f.read(1 << 20), b''):
        digest.update(block)
    hashes[path.relative_to(folder).as_posix()] = digest.hexdigest()
  return hashes


def compare(before, after):
  """Returns (added, removed, changed) relative paths, each sorted."""
  added = sorted(set(after) - set(before))
  removed = sorted(set(before) - set(after))
  changed = sorted(p for p in set(before) & set(after) if before[p] != after[p])
  return added, removed, changed


def _snapshot(args):
  hashes = hash_folder(args.folder)
  snapshot = {'format': _FORMAT, 'folder': str(pathlib.Path(args.folder).resolve()),
              'files': hashes}
  pathlib.Path(args.snapshot).write_text(json.dumps(snapshot, indent=1, sort_keys=True) + '\n',
                                         encoding='utf-8')
  print(f'hash_saves: {len(hashes)} files in {args.folder} -> {args.snapshot}')
  return 0


def _compare(args):
  try:
    snapshot = json.loads(pathlib.Path(args.snapshot).read_text(encoding='utf-8'))
  except (OSError, ValueError) as e:
    print(f'hash_saves: cannot read {args.snapshot}: {e}', file=sys.stderr)
    return 2
  if not isinstance(snapshot, dict) or snapshot.get('format') != _FORMAT or not isinstance(
      snapshot.get('files'), dict):
    print(f'hash_saves: {args.snapshot} is not a snapshot of this tool', file=sys.stderr)
    return 2
  added, removed, changed = compare(snapshot['files'], hash_folder(args.folder))
  for label, paths in (('added', added), ('removed', removed), ('changed', changed)):
    for path in paths:
      print(f'{label}: {path}')
  if added or removed or changed:
    print(f'hash_saves: {args.folder} DIFFERS from {args.snapshot}')
    return 1
  print(f'hash_saves: {args.folder} is unchanged ({len(snapshot["files"])} files)')
  return 0


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  sub = parser.add_subparsers(dest='command', required=True)
  snap = sub.add_parser('snapshot', help='hash every file under a folder')
  snap.add_argument('folder')
  snap.add_argument('snapshot')
  comp = sub.add_parser('compare', help='compare a folder against a snapshot')
  comp.add_argument('snapshot')
  comp.add_argument('folder')
  args = parser.parse_args(argv)
  return _snapshot(args) if args.command == 'snapshot' else _compare(args)


if __name__ == '__main__':
  sys.exit(main())
