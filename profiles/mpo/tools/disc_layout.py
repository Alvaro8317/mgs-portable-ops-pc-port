#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Renders the file listing of a disc as Markdown (names and sizes only).

Reads the output of `mpo --list <iso>` on stdin: one line per entry, either
"DIR/" or "PATH<TAB>SIZE". Writes the "listing" part of
docs/formats/disc-layout.md to stdout; the prose around it is written by hand.

Usage:
  mpo --list game/ULUS-10202.iso | python -I profiles/mpo/tools/disc_layout.py
"""

import collections
import posixpath
import sys

STAGE_PREFIX = 'PSP_GAME/USRDIR/stage/'


def parse_listing(lines):
  """Returns a list of (path, size) for files, skipping directory lines."""
  files = []
  for line_number, line in enumerate(lines, start=1):
    line = line.rstrip('\n')
    if not line or line.endswith('/'):
      continue
    path, separator, size = line.partition('\t')
    if not separator or not size.isdigit():
      raise ValueError(f'line {line_number}: expected "PATH<TAB>SIZE"')
    files.append((path, int(size)))
  return files


def format_size(size):
  return f'{size:,}'.replace(',', ' ')


def render(files):
  """Returns the Markdown listing for the given (path, size) list."""
  out = []
  by_directory = collections.defaultdict(list)
  stages = collections.defaultdict(dict)
  for path, size in files:
    if path.startswith(STAGE_PREFIX):
      stage, _, name = path[len(STAGE_PREFIX):].partition('/')
      stages[stage][name] = size
    else:
      by_directory[posixpath.dirname(path) or '/'].append((path, size))

  out.append('### Resumen por directorio\n')
  out.append('| Directorio | Archivos | Bytes |')
  out.append('|---|---:|---:|')
  totals = collections.defaultdict(lambda: [0, 0])
  for path, size in files:
    directory = posixpath.dirname(path) or '/'
    if directory.startswith(STAGE_PREFIX):
      directory = STAGE_PREFIX + '*'
    totals[directory][0] += 1
    totals[directory][1] += size
  for directory in sorted(totals):
    count, size = totals[directory]
    out.append(f'| `{directory}` | {count} | {format_size(size)} |')
  total_size = sum(size for _, size in files)
  out.append(f'| **Total** | **{len(files)}** | **{format_size(total_size)}** |')

  for directory in sorted(by_directory):
    out.append(f'\n### `{directory}`\n')
    out.append('| Archivo | Bytes |')
    out.append('|---|---:|')
    for path, size in sorted(by_directory[directory]):
      out.append(f'| `{posixpath.basename(path)}` | {format_size(size)} |')

  out.append(f'\n### `{STAGE_PREFIX}<etapa>/` ({len(stages)} etapas)\n')
  out.append('Cada etapa tiene `<etapa>.prx` y `_zar`. Bytes de cada uno:\n')
  out.append('| Etapa | `<etapa>.prx` | `_zar` | Otros |')
  out.append('|---|---:|---:|---|')
  for stage in sorted(stages):
    contents = dict(stages[stage])
    prx = contents.pop(f'{stage}.prx', None)
    zar = contents.pop('_zar', None)
    others = ', '.join(f'`{name}` ({format_size(size)})'
                       for name, size in sorted(contents.items()))
    out.append(f'| `{stage}` | {format_size(prx) if prx is not None else "—"} '
               f'| {format_size(zar) if zar is not None else "—"} | {others} |')
  return '\n'.join(out) + '\n'


def main():
  try:
    files = parse_listing(sys.stdin)
  except ValueError as error:
    print(f'error: {error}', file=sys.stderr)
    return 1
  sys.stdout.write(render(files))
  return 0


if __name__ == '__main__':
  sys.exit(main())
