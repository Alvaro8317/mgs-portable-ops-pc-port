#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares PSPRecomp's automatic analysis with the Ghidra export (F06).

Inputs (both local, under analysis/, never in the repo):
  - the JSON of mpo_analysis_export (profiles/mpo/tools/analysis_export.cpp),
    that is, what psp_analyze / psp_recomp --auto see;
  - the <program>.recomp.json that profiles/mpo/tools/ghidra/run_analysis.sh
    writes, plus the per-step exports in 1-ghidra/, 2-relocations/ and
    3-prologues/ when they exist (to tell which seeding step found a function).

Prints a JSON summary with numbers only (counts, percentages, mnemonic names
and a few addresses), which is what docs/analysis/psp_analyze.md quotes. No
instruction words or game bytes are printed.

Usage:
  python -I profiles/mpo/tools/recomp_compare.py \\
      --export analysis/f06/mgp_main/export.json \\
      --ghidra analysis/out/mgp_main [--program mgp_main]
"""

import argparse
import collections
import json
import os
import sys

# Seeding steps of run_analysis.sh, in order; '' is the final export.
STEPS = ('1-ghidra', '2-relocations', '3-prologues', '')
STEP_NAMES = {'1-ghidra': 'ghidra', '2-relocations': 'relocations',
              '3-prologues': 'prologues', '': 'gaps'}

# OpcodeKind values that psp_recomp lowers to rt.unsupported().
GENERIC_KINDS = ('Vfpu', 'Unsupported', 'Syscall')

_VFPU_SIZES = ('.s', '.p', '.t', '.q')

# PPSSPP Core/MIPS/MIPSTables.cpp (v1.20.4): tableVFPU4Jump, tableVFPU7 and
# tableVFPU9, indexed by bits 21-25 and 16-20. Only names, for reporting.
_VFPU7 = {0: 'vrnds', 1: 'vrndi', 2: 'vrndf1', 3: 'vrndf2', 18: 'vf2h',
          19: 'vh2f', 22: 'vsbz', 23: 'vlgb', 24: 'vuc2i', 25: 'vc2i',
          26: 'vus2i', 27: 'vs2i', 28: 'vi2uc', 29: 'vi2c', 30: 'vi2us',
          31: 'vi2s'}
_VFPU9 = {0: 'vsrt1', 1: 'vsrt2', 2: 'vbfy1', 3: 'vbfy2', 4: 'vocp',
          5: 'vsocp', 6: 'vfad', 7: 'vavg', 8: 'vsrt3', 9: 'vsrt4',
          10: 'vsgn', 16: 'vmfvc', 17: 'vmtvc', 25: 'vt4444', 26: 'vt5551',
          27: 'vt5650'}
_VFPU0 = {0: 'vadd', 1: 'vsub', 2: 'vsbn', 7: 'vdiv'}
_VFPU1 = {0: 'vmul', 1: 'vdot', 2: 'vscl', 4: 'vhdp', 5: 'vcrs', 6: 'vdet'}
_VFPU3 = {0: 'vcmp', 2: 'vmin', 3: 'vmax', 5: 'vscmp', 6: 'vsge', 7: 'vslt'}
_SPECIAL = {0x0C: 'syscall', 0x0D: 'break', 0x1C: 'madd', 0x1D: 'maddu',
            0x2E: 'msub', 0x2F: 'msubu'}
_SPECIAL2 = {0x00: 'halt', 0x24: 'mfic', 0x26: 'mtic'}


class CompareError(Exception):
  """Raised when an input does not have the expected shape."""


def name_word(word):
  """Names an Allegrex word that PSPRecomp's decoder leaves generic.

  Covers the encodings that decode_allegrex() maps to Vfpu, Unsupported or
  Syscall; anything else gets a descriptive placeholder (opXX...).
  """
  op = word >> 26
  size = _VFPU_SIZES[((word >> 7) & 1) | (((word >> 15) & 1) << 1)]
  group = (word >> 21) & 0x1F
  sub = (word >> 16) & 0x1F
  if op == 0x00:
    return _SPECIAL.get(word & 0x3F, f'special.fn{word & 0x3F:02x}')
  if op == 0x1C:
    return _SPECIAL2.get(word & 0x3F, f'special2.fn{word & 0x3F:02x}')
  if op == 0x1F:
    return f'special3.fn{word & 0x3F:02x}.sa{(word >> 6) & 0x1F:02x}'
  if op == 0x10:
    return f'cop0.rs{group:02x}'
  if op == 0x11:
    return f'cop1.rs{group:02x}.fn{word & 0x3F:02x}'
  if op == 0x12:
    if group == 0x03 and word & 0x80:
      return 'mfvc'
    if group == 0x07 and word & 0x80:
      return 'mtvc'
    return f'cop2.rs{group:02x}'
  if op == 0x18:
    return _VFPU0.get((word >> 23) & 7, 'vfpu0.invalid') + size
  if op == 0x19:
    return _VFPU1.get((word >> 23) & 7, 'vfpu1.invalid') + size
  if op == 0x1B:
    return _VFPU3.get((word >> 23) & 7, 'vfpu3.invalid') + size
  if op == 0x34:
    if group == 1:
      return _VFPU7.get(sub, f'vfpu7.{sub}') + size
    if group == 2:
      return _VFPU9.get(sub, f'vfpu9.{sub}') + size
    if group >= 24:
      return 'vwbn' + size
    return f'vfpu4.g{group}.{sub}' + size
  if op == 0x35:
    return 'lvr.q' if word & 2 else 'lvl.q'
  if op == 0x3D:
    return 'svr.q' if word & 2 else 'svl.q'
  if op == 0x3C:
    if group == 28:
      return f'vfpu6.matrix{(word >> 16) & 0xF}'
    return f'vfpu6.g{group}'
  return f'op{op:02x}'


def _hex(text):
  value = int(text, 16)
  if not 0 <= value <= 0xFFFFFFFF:
    raise CompareError(f'address out of range: {text}')
  return value


def _load(path):
  with open(path, encoding='utf-8') as json_file:
    return json.load(json_file)


def _words(start, end):
  if end < start or (end - start) > 0x04000000:
    raise CompareError(f'bad range {start:#x}-{end:#x}')
  return range(start, end, 4)


class Ghidra:
  """The Ghidra side: function entries, code words and jump tables."""

  def __init__(self, recomp):
    self.exec_blocks = [(_hex(b['start']), _hex(b['end']))
                        for b in recomp['exec_blocks']]
    self.entries = {}
    self.imports = set()
    self.code = set()
    self.owner = {}
    for function in recomp['functions']:
      entry = _hex(function['addr'])
      if not self.in_exec(entry):
        continue  # e.g. the stand-in block of a stage (MpoExternalBlock)
      self.entries[entry] = function['name']
      if function.get('import'):
        self.imports.add(entry)
      for start, end in function['ranges']:
        for address in _words(_hex(start), _hex(end)):
          self.code.add(address)
          self.owner.setdefault(address, entry)
    self.jump_tables = [
        (_hex(t['at']), _hex(t['function']), [_hex(x) for x in t['targets']])
        for t in recomp['jump_tables']]
    self.indirect_calls = [(_hex(c['at']), [_hex(x) for x in c['targets']])
                           for c in recomp['indirect_calls']]

  def in_exec(self, address):
    return any(start <= address < end for start, end in self.exec_blocks)


class Psp:
  """The PSPRecomp side, from mpo_analysis_export."""

  def __init__(self, export):
    self.ranges = [(_hex(a), _hex(b)) for a, b in export['executable_ranges']]
    self.seeds = {_hex(a): source for a, source in export['seeds']}
    self.entries = {}
    self.code = set()
    self.indirect_sites = set()
    self.truncated = 0
    for function in export['functions']:
      entry = _hex(function['entry'])
      self.entries[entry] = function
      self.truncated += 1 if function['truncated'] else 0
      for site in function['indirect_call_sites']:
        self.indirect_sites.add(_hex(site))
      for start, end in function['runs']:
        self.code.update(_words(_hex(start), _hex(end)))
    self.entry_labels = {_hex(a) for a in export['entry_labels']}
    self.sections = [(s['name'], _hex(s['start']), _hex(s['end']), s['flags'])
                     for s in export['sections']]
    self.kind_names = {int(k): v for k, v in export['kind_names'].items()}
    self.kinds = {}
    for block in export['census']:
      start = _hex(block['start'])
      text = block['kinds']
      if len(text) % 2:
        raise CompareError('census string with odd length')
      for index in range(0, len(text), 2):
        self.kinds[start + index * 2] = int(text[index:index + 2], 16)
    self.generic = {_hex(a): (_hex(w), m) for a, w, m in export['generic_words']}

  def section_of(self, address):
    for name, start, end, _ in self.sections:
      if start <= address < end:
        return name
    return '<none>'


def _step_of_entries(ghidra_dir, program, final):
  """Maps each final Ghidra entry to the first seeding step that had it."""
  step_of = {}
  for step in STEPS:
    path = os.path.join(ghidra_dir, step, f'{program}.recomp.json')
    if not os.path.exists(path):
      continue
    for function in _load(path)['functions']:
      step_of.setdefault(_hex(function['addr']), STEP_NAMES[step])
  return {entry: step_of.get(entry, 'unknown') for entry in final.entries}


def _counter(items):
  return dict(collections.Counter(items).most_common())


def compare(ghidra, psp, steps):
  """Builds the summary dictionary."""
  g_entries = set(ghidra.entries)
  p_entries = set(psp.entries)
  both = g_entries & p_entries
  g_only = g_entries - p_entries
  p_only = p_entries - g_entries

  # Ghidra-only entries: still emitted inside another psp function (its code
  # is covered, but it is not an entry), or missing altogether.
  g_only_kind = collections.Counter()
  g_only_by_step = collections.Counter()
  g_only_missing_by_step = collections.Counter()
  for entry in g_only:
    step = steps.get(entry, 'unknown')
    g_only_by_step[step] += 1
    if entry in ghidra.imports:
      g_only_kind['import_stub'] += 1
    elif entry in psp.code:
      g_only_kind['covered_not_entry' if entry not in psp.entry_labels
                  else 'covered_as_block_entry'] += 1
    else:
      g_only_kind['not_covered'] += 1
      g_only_missing_by_step[step] += 1

  jump_targets = {t for _, _, targets in ghidra.jump_tables for t in targets}
  p_only_where = collections.Counter()
  p_only_source = collections.Counter()
  p_only_outside_section = collections.Counter()
  for entry in p_only:
    p_only_source[psp.seeds.get(entry, '?')] += 1
    if entry in jump_targets:
      p_only_where['jump_table_target'] += 1
    elif entry in ghidra.code:
      p_only_where['inside_ghidra_function'] += 1
    elif ghidra.in_exec(entry):
      p_only_where['ghidra_exec_not_in_function'] += 1
    else:
      p_only_where['outside_ghidra_exec'] += 1
      p_only_outside_section[psp.section_of(entry)] += 1

  # Coverage in words. Ghidra's code is the reference for real code.
  g_code = ghidra.code
  p_code = psp.code
  g_not_p = g_code - p_code
  p_not_g = p_code - g_code
  g_not_p_by_step = collections.Counter(
      steps.get(ghidra.owner[a], 'unknown') for a in g_not_p)
  p_not_g_by_section = collections.Counter(psp.section_of(a) for a in p_not_g)
  p_not_g_in_exec = sum(1 for a in p_not_g if ghidra.in_exec(a))

  # Jump tables: a jr can stay in the generated unit only if every target is a
  # dispatcher entry (block entry or seed) and the jr itself is emitted.
  dispatch = psp.entry_labels | set(psp.seeds)
  tables_ok = sum(1 for at, _, targets in ghidra.jump_tables
                  if targets and at in p_code and
                  all(t in dispatch for t in targets))
  tables_unresolved = [f'{at:#010x}' for at, _, targets in ghidra.jump_tables
                       if not targets]
  tables_jr_missing = sum(1 for at, _, _ in ghidra.jump_tables
                          if at not in p_code)
  targets_total = sum(len(t) for _, _, t in ghidra.jump_tables)
  targets_seeded = sum(1 for _, _, ts in ghidra.jump_tables for t in ts
                       if t in psp.seeds)

  # Decoder census over Ghidra's code.
  kinds = collections.Counter()
  generic_in_code = collections.Counter()
  generic_in_psp = collections.Counter()
  for address in g_code:
    kind = psp.kinds.get(address)
    if kind is None:
      continue
    kinds[psp.kind_names.get(kind, str(kind))] += 1
    if address in psp.generic:
      word, _ = psp.generic[address]
      name = name_word(word)
      generic_in_code[name] += 1
      if address in p_code:
        generic_in_psp[name] += 1
  generic_outside = sum(1 for a in psp.generic if a not in g_code)

  # Control flow in a delay slot (psp_recomp emits rt.unsupported for it).
  branchy = {'beq', 'bne', 'beql', 'bnel', 'blez', 'bgtz', 'blezl', 'bgtzl',
             'bltz', 'bgez', 'bltzl', 'bgezl', 'bltzal', 'bgezal', 'bltzall',
             'bgezall', 'j', 'jal', 'jr', 'jalr', 'bc1f', 'bc1t', 'bc1fl',
             'bc1tl', 'bvf', 'bvt', 'bvfl', 'bvtl'}
  delay_cf = 0
  for address in g_code:
    kind = psp.kinds.get(address)
    slot = psp.kinds.get(address + 4)
    if kind is None or slot is None:
      continue
    if (psp.kind_names.get(kind) in branchy and address + 4 in g_code and
        psp.kind_names.get(slot) in branchy | {'syscall'}):
      delay_cf += 1

  return {
      'functions': {
          'ghidra': len(g_entries),
          'ghidra_imports': len(ghidra.imports),
          'psp_analyze': len(p_entries),
          'both': len(both),
          'ghidra_only': len(g_only),
          'psp_only': len(p_entries - g_entries),
          'psp_truncated': psp.truncated,
          'ghidra_by_step': _counter(steps.values()),
          'both_by_step': _counter(steps[e] for e in both),
          'ghidra_only_by_step': dict(g_only_by_step.most_common()),
          'ghidra_only_kind': dict(g_only_kind.most_common()),
          'ghidra_only_not_covered_by_step':
              dict(g_only_missing_by_step.most_common()),
          'psp_seed_sources': _counter(psp.seeds.values()),
          'psp_only_seed_source': dict(p_only_source.most_common()),
          'psp_only_where': dict(p_only_where.most_common()),
          'psp_only_outside_ghidra_exec_by_section':
              dict(p_only_outside_section.most_common()),
      },
      'coverage_words': {
          'ghidra_code': len(g_code),
          'psp_code': len(p_code),
          'both': len(g_code & p_code),
          'ghidra_not_psp': len(g_not_p),
          'ghidra_not_psp_percent':
              round(100.0 * len(g_not_p) / len(g_code), 3) if g_code else 0.0,
          'ghidra_not_psp_by_step_of_owner':
              dict(g_not_p_by_step.most_common()),
          'psp_not_ghidra': len(p_not_g),
          'psp_not_ghidra_in_ghidra_exec': p_not_g_in_exec,
          'psp_not_ghidra_by_section': dict(p_not_g_by_section.most_common()),
      },
      'jump_tables': {
          'ghidra_tables': len(ghidra.jump_tables),
          'ghidra_unresolved': tables_unresolved,
          'targets': targets_total,
          'targets_that_are_psp_seeds': targets_seeded,
          'tables_fully_dispatchable': tables_ok,
          'tables_with_jr_not_emitted': tables_jr_missing,
      },
      'indirect_calls': {
          'psp_jalr_sites': len(psp.indirect_sites),
          'psp_jalr_sites_in_ghidra_code':
              sum(1 for s in psp.indirect_sites if s in g_code),
          'ghidra_resolved_indirect_calls': len(ghidra.indirect_calls),
      },
      'decoder': {
          'kinds_in_ghidra_code': dict(kinds.most_common()),
          'generic_in_ghidra_code': dict(generic_in_code.most_common()),
          'generic_in_ghidra_code_and_psp': dict(generic_in_psp.most_common()),
          'generic_words_outside_ghidra_code': generic_outside,
          'control_flow_in_delay_slot': delay_cf,
      },
      'psp_executable_ranges': [[f'{a:#010x}', f'{b:#010x}']
                                for a, b in psp.ranges],
  }


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--export', required=True,
                      help='JSON written by mpo_analysis_export')
  parser.add_argument('--ghidra', required=True,
                      help='run_analysis.sh output directory (out/<program>)')
  parser.add_argument('--program',
                      help='program name (default: the directory name)')
  args = parser.parse_args(argv)
  program = args.program or os.path.basename(os.path.normpath(args.ghidra))
  try:
    final = _load(os.path.join(args.ghidra, f'{program}.recomp.json'))
    ghidra = Ghidra(final)
    psp = Psp(_load(args.export))
    steps = _step_of_entries(args.ghidra, program, ghidra)
    summary = compare(ghidra, psp, steps)
  except (OSError, ValueError, KeyError, TypeError, CompareError) as error:
    print(f'recomp_compare: {error}', file=sys.stderr)
    return 1
  json.dump(summary, sys.stdout, indent=2)
  print()
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
