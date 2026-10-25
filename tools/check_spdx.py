#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks that every project source file starts with the SPDX license tag.

The tag must be on the first line, or on the second one when the first line is
a shebang (CLAUDE.md §11). The PSPRecomp submodule (MIT) and third-party code
keep their own headers and are not checked.

Usage:
  python -I tools/check_spdx.py [--root REPO_ROOT]
"""

import argparse
import pathlib
import subprocess
import sys

EXPECTED_TAG = 'SPDX-License-Identifier: GPL-3.0-or-later'

# Paths owned by someone else: the framework submodule and vendored code.
EXCLUDED_PREFIXES = ('framework/', 'third_party/')

CHECKED_SUFFIXES = frozenset({
    '.bash',
    '.c',
    '.cc',
    '.cmake',
    '.comp',
    '.cpp',
    '.cxx',
    '.frag',
    '.glsl',
    '.h',
    '.hh',
    '.hpp',
    '.hxx',
    '.inl',
    '.java',
    '.ps1',
    '.py',
    '.sh',
    '.toml',
    '.vert',
    '.yaml',
    '.yml',
})

CHECKED_NAMES = frozenset({'CMakeLists.txt'})


def is_checked(relative_path):
  """Returns whether a repo-relative POSIX path must carry the SPDX tag."""
  if relative_path.startswith(EXCLUDED_PREFIXES):
    return False
  path = pathlib.PurePosixPath(relative_path)
  return path.name in CHECKED_NAMES or path.suffix in CHECKED_SUFFIXES


def has_valid_header(text):
  """Returns whether the text carries the SPDX tag in its header line.

  Empty files are accepted, since they contain nothing to license.
  """
  lines = text.splitlines()
  if not lines:
    return True
  header = lines[0]
  if header.startswith('#!'):
    header = lines[1] if len(lines) > 1 else ''
  return EXPECTED_TAG in header


def list_candidate_files(root):
  """Lists tracked and untracked, non-ignored files below root.

  Untracked files are included so the check also catches new files before they
  are committed.
  """
  result = subprocess.run(
      ['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
      cwd=root,
      check=True,
      capture_output=True,
  )
  names = result.stdout.decode('utf-8').split('\0')
  return sorted({name for name in names if name})


def find_violations(root):
  """Returns the repo-relative paths that lack a valid SPDX header."""
  violations = []
  for relative_path in list_candidate_files(root):
    if not is_checked(relative_path):
      continue
    path = root / relative_path
    # Submodules are listed as directories; deleted files may still be listed.
    if not path.is_file():
      continue
    text = path.read_bytes().decode('utf-8', errors='replace')
    if not has_valid_header(text):
      violations.append(relative_path)
  return violations


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument(
      '--root',
      type=pathlib.Path,
      default=pathlib.Path(__file__).resolve().parent.parent,
      help='repository root (defaults to the parent of tools/)',
  )
  args = parser.parse_args(argv)

  try:
    violations = find_violations(args.root)
  except (OSError, subprocess.CalledProcessError) as error:
    print(f'error: cannot list files under {args.root}: {error}',
          file=sys.stderr)
    return 2

  if violations:
    print(f'Missing "{EXPECTED_TAG}" header in:', file=sys.stderr)
    for relative_path in violations:
      print(f'  {relative_path}', file=sys.stderr)
    return 1
  print('SPDX check passed.')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
