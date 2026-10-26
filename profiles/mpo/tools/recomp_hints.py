#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turns the Ghidra export into analysis hints for PSPRecomp (F06 design).

Reads the <program>.recomp.json that profiles/mpo/tools/ghidra/run_analysis.sh
writes and prints the text format of psprecomp::load_analysis_hints() (the
generic framework patch in framework/patches/, see
docs/analysis/psp_analyze.md):

  code <start> <end>                 one per Ghidra executable block
  seed <address> ghidra              one per Ghidra function entry
  jumptable <jr> <target>...         one per resolved Ghidra jump table

Hand-written profile hints (profiles/mpo/config/hints/<program>.txt, same
format) are validated and appended with --extra; that is where the jump tables
Ghidra could not resolve go. The output only holds addresses, but it is made
from the user's executable, so it is written under analysis/.

Usage:
  python -I profiles/mpo/tools/recomp_hints.py \\
      analysis/out/mgp_main/mgp_main.recomp.json \\
      --extra profiles/mpo/config/hints/mgp_main.txt \\
      --output analysis/f06/mgp_main/mgp_main.hints.txt
"""

import argparse
import json
import sys

_DIRECTIVES = {'code': (2, 2), 'seed': (1, 2), 'jumptable': (2, None)}


class HintsError(Exception):
  """Raised when an input is malformed."""


def _hex(text):
  try:
    value = int(text, 16)
  except ValueError:
    raise HintsError(f'not a hexadecimal number: {text!r}') from None
  if not 0 <= value <= 0xFFFFFFFF:
    raise HintsError(f'address out of range: {text}')
  return value


def _fmt(value):
  return f'0x{value:08X}'


def parse_hints(text, origin='<hints>'):
  """Parses the hints text format; returns a list of (directive, args)."""
  directives = []
  for number, line in enumerate(text.splitlines(), 1):
    line = line.split('#', 1)[0].split()
    if not line:
      continue
    name, args = line[0], line[1:]
    if name not in _DIRECTIVES:
      raise HintsError(f'{origin}:{number}: unknown directive {name!r}')
    low, high = _DIRECTIVES[name]
    if len(args) < low or (high is not None and len(args) > high):
      raise HintsError(f'{origin}:{number}: wrong number of fields')
    values = [_hex(a) for a in args[:1 if name == 'seed' else None]]
    if name == 'seed':
      values.append(args[1] if len(args) > 1 else 'hint')
    if name == 'code' and values[1] <= values[0]:
      raise HintsError(f'{origin}:{number}: empty code range')
    if any(isinstance(v, int) and v & 3 for v in values):
      raise HintsError(f'{origin}:{number}: unaligned address')
    directives.append((name, values))
  return directives


def from_recomp(recomp):
  """Builds the directives for one Ghidra export."""
  directives = []
  blocks = []
  for block in recomp['exec_blocks']:
    start, end = _hex(block['start']), _hex(block['end'])
    if end <= start:
      raise HintsError(f'empty executable block {block["name"]}')
    blocks.append((start, end))
    directives.append(('code', [start, end]))

  def in_code(address):
    return any(start <= address < end for start, end in blocks)

  for function in sorted(recomp['functions'], key=lambda f: _hex(f['addr'])):
    address = _hex(function['addr'])
    if in_code(address):  # skips the stand-in block of a stage
      directives.append(('seed', [address, 'ghidra']))
  for table in sorted(recomp['jump_tables'], key=lambda t: _hex(t['at'])):
    targets = [_hex(t) for t in table['targets']]
    if targets:  # an unresolved table needs a hand-written hint
      directives.append(('jumptable', [_hex(table['at'])] + targets))
  return directives


def render(directives):
  lines = []
  for name, values in directives:
    fields = [_fmt(v) if isinstance(v, int) else v for v in values]
    lines.append(' '.join([name] + fields))
  return '\n'.join(lines) + '\n'


def summary(directives):
  counts = {}
  for name, _ in directives:
    counts[name] = counts.get(name, 0) + 1
  return counts


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('recomp', help='<program>.recomp.json from Ghidra')
  parser.add_argument('--extra', action='append', default=[],
                      help='hand-written hints to validate and append')
  parser.add_argument('--output', required=True, help='hints file to write')
  args = parser.parse_args(argv)
  try:
    with open(args.recomp, encoding='utf-8') as recomp_file:
      directives = from_recomp(json.load(recomp_file))
    for path in args.extra:
      with open(path, encoding='utf-8') as extra_file:
        directives += parse_hints(extra_file.read(), path)
    text = render(directives)
    parse_hints(text, args.output)  # what we write must read back
    with open(args.output, 'w', encoding='utf-8') as output:
      output.write(text)
  except (OSError, ValueError, KeyError, TypeError, HintsError) as error:
    print(f'recomp_hints: {error}', file=sys.stderr)
    return 1
  print(f'recomp_hints: {summary(directives)} -> {args.output}')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
