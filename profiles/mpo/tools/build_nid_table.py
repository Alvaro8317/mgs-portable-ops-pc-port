#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds a PSP NID table from PPSSPP's HLE sources (Core/HLE/*.cpp).

Each row of the output TSV is one entry of a PPSSPP `HLEFunction` table:

  nid  library  name  implemented  source_file

- `library` is the PSP library name the table is registered under
  (`RegisterHLEModule("X", ARRAY_SIZE(t), t)`, `std::size(t)` or the
  `{"X", ARRAY_SIZE(t), t}` entries of HLETables.cpp). One table can be
  registered under several libraries (`sceCtrl` and `sceCtrl_driver`).
- `implemented` is 1 when PPSSPP gives a handler and 0 when the entry is
  `nullptr` (known name, no implementation).
- The module entry points and variables that every PRX exports
  (`module_start`, `module_info`, ...) are added under the pseudo-library
  `syslib`, from the `NID_MODULE_*` constants of sceKernelModule.cpp.

The same NID can appear under several libraries (for example, the
`ThreadManForUser` and `ThreadManForKernel` copies), so the importer should
look it up by (library, NID) first and fall back to the NID alone.

The PPSSPP checkout is pinned by the caller (CLAUDE.md §5: tag v1.20.4). The
output is a derived table of names, not game data, but it is regenerated
locally into analysis/ and not versioned.

Usage:
  python -I profiles/mpo/tools/build_nid_table.py \
      <ppsspp>/Core/HLE analysis/nids/ppsspp-v1.20.4.tsv

  # Download Core/HLE/*.cpp of the pinned commit into the directory first
  # (GitHub API + raw.githubusercontent.com; the directory must be empty).
  python -I profiles/mpo/tools/build_nid_table.py --fetch \
      analysis/ppsspp-v1.20.4/Core/HLE analysis/nids/ppsspp-v1.20.4.tsv
"""

import argparse
import json
import os
import re
import sys
import urllib.request

# PPSSPP v1.20.4 (the same pin as third_party/ppsspp_crypto).
PPSSPP_COMMIT = 'fa50bb1976065c4f8b1b47af227d367fe9771555'
_LIST_URL = ('https://api.github.com/repos/hrydgard/ppsspp/contents/Core/HLE'
             '?ref={commit}')
_RAW_URL = ('https://raw.githubusercontent.com/hrydgard/ppsspp/{commit}/'
            'Core/HLE/{name}')
_SAFE_NAME_RE = re.compile(r'^[A-Za-z0-9_]+\.cpp$')

_TABLE_RE = re.compile(
    r'HLEFunction\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\s*\};', re.DOTALL)
_ENTRY_RE = re.compile(
    r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,\s*([^,]+?)\s*,\s*"([^"]*)"')
_SIZE = r'(?:ARRAY_SIZE|std::size)\((\w+)\)'
_REGISTER_RE = re.compile(
    r'RegisterHLEModule\(\s*"([^"]+)"\s*,\s*' + _SIZE + r'\s*,\s*\2\s*\)')
_MODULE_LIST_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*' + _SIZE + r'\s*,\s*\2\s*\}')
_SYSLIB_RE = re.compile(r'\bNID_(MODULE_\w+)\s*=\s*0[xX]([0-9A-Fa-f]{8})')
_NO_HANDLER = ('nullptr', '0', 'NULL')


def parse_sources(hle_dir):
  """Returns (rows, table_count) for every .cpp file under `hle_dir`."""
  tables = {}  # table identifier -> list of (nid, name, implemented, file)
  libraries = {}  # table identifier -> list of library names
  syslib = []
  for file_name in sorted(os.listdir(hle_dir)):
    if not file_name.endswith('.cpp'):
      continue
    with open(os.path.join(hle_dir, file_name), encoding='utf-8',
              errors='replace') as source_file:
      source = source_file.read()
    for regex in (_REGISTER_RE, _MODULE_LIST_RE):
      for match in regex.finditer(source):
        names = libraries.setdefault(match.group(2), [])
        if match.group(1) not in names:
          names.append(match.group(1))
    for match in _SYSLIB_RE.finditer(source):
      syslib.append((int(match.group(2), 16), 'syslib',
                     match.group(1).lower(), True, file_name))
    for table in _TABLE_RE.finditer(source):
      entries = tables.setdefault(table.group(1), [])
      for entry in _ENTRY_RE.finditer(table.group(2)):
        handler = entry.group(2).strip()
        entries.append((int(entry.group(1), 16), entry.group(3),
                        handler not in _NO_HANDLER, file_name))
  rows = list(syslib)
  for table_name, entries in tables.items():
    # Tables that are never registered are not importable and are skipped.
    for library in libraries.get(table_name, ()):
      for nid, name, implemented, file_name in entries:
        rows.append((nid, library, name, implemented, file_name))
  rows.sort(key=lambda row: (row[1], row[0]))
  return rows, len(tables)


def fetch_sources(hle_dir, commit):
  """Downloads Core/HLE/*.cpp of PPSSPP at `commit` into `hle_dir`."""
  os.makedirs(hle_dir, exist_ok=True)
  if os.listdir(hle_dir):
    raise ValueError(f'{hle_dir} is not empty')
  with urllib.request.urlopen(_LIST_URL.format(commit=commit),
                              timeout=60) as response:
    listing = json.load(response)
  names = [entry['name'] for entry in listing
           if entry.get('type') == 'file' and
           _SAFE_NAME_RE.match(entry.get('name', ''))]
  for name in names:
    with urllib.request.urlopen(_RAW_URL.format(commit=commit, name=name),
                                timeout=60) as response:
      data = response.read()
    with open(os.path.join(hle_dir, name), 'wb') as source_file:
      source_file.write(data)
  return len(names)


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('hle_dir', help='PPSSPP Core/HLE directory')
  parser.add_argument('output', help='output TSV')
  parser.add_argument('--fetch', action='store_true',
                      help='download Core/HLE/*.cpp into hle_dir first')
  parser.add_argument('--commit', default=PPSSPP_COMMIT,
                      help='PPSSPP commit for --fetch (default: v1.20.4)')
  args = parser.parse_args(argv)
  if args.fetch:
    try:
      count = fetch_sources(args.hle_dir, args.commit)
    except (OSError, ValueError) as error:
      print(f'error: fetch failed: {error}', file=sys.stderr)
      return 1
    print(f'fetched {count} files from PPSSPP {args.commit}')
  rows, table_count = parse_sources(args.hle_dir)
  if not rows:
    print(f'error: no HLE tables found in {args.hle_dir}', file=sys.stderr)
    return 1
  os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
  with open(args.output, 'w', encoding='utf-8', newline='\n') as output:
    for nid, library, name, implemented, file_name in rows:
      output.write(f'{nid:08X}\t{library}\t{name}\t{int(implemented)}\t'
                   f'{file_name}\n')
  libraries = {row[1] for row in rows}
  implemented = sum(1 for row in rows if row[3])
  print(f'{len(rows)} entries ({implemented} implemented), '
        f'{len({row[0] for row in rows})} distinct NIDs, '
        f'{len(libraries)} libraries, {table_count} tables')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
