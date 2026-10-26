#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for recomp_hints.py, recomp_compare.py and mpo_analysis_export (F06).

Synthetic inputs only. The end-to-end case runs the C++ exporter on a
synthetic PRX when MPO_ANALYSIS_EXPORT names the binary (ctest sets it).
"""

import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load(name):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


recomp_compare = _load('recomp_compare')
recomp_hints = _load('recomp_hints')
synthetic_prx = _load('synthetic_prx')

_BASE = 0x08804000


def _h(value):
  return f'0x{value:08X}'


def _ghidra(functions, jump_tables=(), blocks=((_BASE, _BASE + 0x28),),
            imports=()):
  """A minimal <program>.recomp.json."""
  return {
      'program': 'synthetic',
      'image_base': _h(_BASE),
      'exec_blocks': [{'name': '.text', 'start': _h(s), 'end': _h(e)}
                      for s, e in blocks],
      'functions': [{'addr': _h(start), 'name': f'FUN_{start:08X}',
                     'source': 'DEFAULT',
                     'import': 'lib:00000000' if start in imports else None,
                     'ranges': [[_h(start), _h(end)]]}
                    for start, end in functions],
      'jump_tables': [{'at': _h(at), 'function': _h(owner),
                       'targets': [_h(t) for t in targets]}
                      for at, owner, targets in jump_tables],
      'indirect_calls': [],
  }


class NameWordTest(unittest.TestCase):

  def test_generic_encodings(self):
    cases = {
        0x0000000D: 'break',
        0x0000000C: 'syscall',
        0x0000001C: 'madd',
        0xD03F8080: 'vi2s.q',   # VFPU7 sub 31, size .q
        0xD03F0080: 'vi2s.p',
        0xD03C8080: 'vi2uc.q',
        0x66808000: 'vcrs.t',   # VFPU1 op 5, size .t
        0xD04A8000: 'vsgn.t',   # VFPU9 sub 10
        0xD0500000: 'vmfvc.s',
        0xD4000000: 'lvl.q',
        0xD4000002: 'lvr.q',
        0xF4000000: 'svl.q',
        0x48600080: 'mfvc',
        0x70000000: 'halt',
        0x61000000: 'vsbn.s',
    }
    for word, name in cases.items():
      with self.subTest(word=hex(word)):
        self.assertEqual(recomp_compare.name_word(word), name)


class HintsTest(unittest.TestCase):

  def test_from_recomp(self):
    recomp = _ghidra(
        functions=[(_BASE, _BASE + 0x10), (_BASE + 0x10, _BASE + 0x28),
                   (0x08000000, 0x08000008)],  # outside: a stand-in block
        jump_tables=[(_BASE + 0x8, _BASE, [_BASE + 0x10, _BASE + 0x18]),
                     (_BASE + 0x20, _BASE + 0x10, [])])  # unresolved
    directives = recomp_hints.from_recomp(recomp)
    self.assertEqual(directives, [
        ('code', [_BASE, _BASE + 0x28]),
        ('seed', [_BASE, 'ghidra']),
        ('seed', [_BASE + 0x10, 'ghidra']),
        ('jumptable', [_BASE + 0x8, _BASE + 0x10, _BASE + 0x18]),
    ])
    text = recomp_hints.render(directives)
    self.assertIn('jumptable 0x08804008 0x08804010 0x08804018', text)
    self.assertEqual(recomp_hints.parse_hints(text), directives)

  def test_parse(self):
    text = ('# comment\n\ncode 08804000 0x08804010\n'
            'seed 0x08804004 extra  # trailing\nseed 0x08804008\n'
            'jumptable 0x08804000 0x08804004\n'
            'patch 0x08804008 0x03E00009\n')
    self.assertEqual(recomp_hints.parse_hints(text), [
        ('code', [_BASE, _BASE + 0x10]),
        ('seed', [_BASE + 4, 'extra']),
        ('seed', [_BASE + 8, 'hint']),
        ('jumptable', [_BASE, _BASE + 4]),
        ('patch', [_BASE + 8, 0x03E00009]),  # the word need not be aligned
    ])

  def test_parse_errors(self):
    bad = ['frob 0x0', 'code 0x10', 'code 0x20 0x10', 'seed nothex',
           'seed 0x08804001', 'jumptable 0x08804000', 'seed 0x1FFFFFFFF',
           'seed 0x0 a b', 'patch 0x08804000', 'patch 0x08804002 0x0',
           'patch 0x08804000 0x0 0x0']
    for line in bad:
      with self.subTest(line=line):
        with self.assertRaises(recomp_hints.HintsError):
          recomp_hints.parse_hints(line)

  def test_main_writes_and_rejects(self):
    with tempfile.TemporaryDirectory() as tmp:
      tmp = pathlib.Path(tmp)
      recomp = tmp / 'p.recomp.json'
      recomp.write_text(json.dumps(_ghidra([(_BASE, _BASE + 0x28)])))
      extra = tmp / 'extra.txt'
      extra.write_text('jumptable 0x08804010 0x08804018\n')
      out = tmp / 'hints.txt'
      self.assertEqual(recomp_hints.main(
          [str(recomp), '--extra', str(extra), '--output', str(out)]), 0)
      self.assertIn('jumptable 0x08804010 0x08804018', out.read_text())
      extra.write_text('jumptable 0x08804010\n')
      self.assertEqual(recomp_hints.main(
          [str(recomp), '--extra', str(extra), '--output', str(out)]), 1)


def _export(functions, seeds, kinds, generic=(), entry_labels=(),
            sections=(('.text', _BASE, _BASE + 0x28, 6),
                      ('.rodata', _BASE + 0x28, _BASE + 0x30, 2))):
  """A minimal mpo_analysis_export document."""
  start = _BASE
  return {
      'executable_ranges': [[_h(_BASE), _h(_BASE + 4 * len(kinds))]],
      'sections': [{'name': n, 'start': _h(s), 'end': _h(e), 'flags': f}
                   for n, s, e, f in sections],
      'seeds': [[_h(a), source] for a, source in seeds],
      'functions': [{'entry': _h(entry), 'block_entries': 1,
                     'direct_calls': 0, 'unsupported': 0,
                     'truncated': False, 'indirect_call_sites': [],
                     'runs': [[_h(s), _h(e)] for s, e in runs]}
                    for entry, runs in functions],
      'entry_labels': [_h(a) for a in entry_labels],
      'kind_names': {'0': 'nop', '1': 'addiu', '2': 'jr', '3': 'special?',
                     '4': 'jal'},
      'census': [{'start': _h(start),
                  'kinds': ''.join(f'{k:02x}' for k in kinds)}],
      'generic_words': [[_h(a), _h(w), 'special?'] for a, w in generic],
  }


class CompareTest(unittest.TestCase):

  def test_compare(self):
    # Ghidra: three functions in .text and a jump table. psp_analyze: the
    # first two, one seed in .rodata and the last Ghidra function missing.
    ghidra = recomp_compare.Ghidra(_ghidra(
        functions=[(_BASE, _BASE + 0x10), (_BASE + 0x10, _BASE + 0x1C),
                   (_BASE + 0x1C, _BASE + 0x28)],
        jump_tables=[(_BASE + 0x8, _BASE, [_BASE + 0x10])]))
    kinds = [4, 0, 2, 0, 3, 2, 0, 1, 2, 0, 1, 1]
    psp = recomp_compare.Psp(_export(
        functions=[(_BASE, [(_BASE, _BASE + 0x10)]),
                   (_BASE + 0x10, [(_BASE + 0x10, _BASE + 0x1C)]),
                   (_BASE + 0x28, [(_BASE + 0x28, _BASE + 0x30)])],
        seeds=[(_BASE, 'elf_entry'), (_BASE + 0x10, 'direct_jal_target'),
               (_BASE + 0x28, 'materialized_code_pointer')],
        kinds=kinds, generic=[(_BASE + 0x10, 0x0000000D)],
        entry_labels=[_BASE, _BASE + 0x10, _BASE + 0x28]))
    steps = {_BASE: 'ghidra', _BASE + 0x10: 'ghidra', _BASE + 0x1C: 'gaps'}
    summary = recomp_compare.compare(ghidra, psp, steps)
    functions = summary['functions']
    self.assertEqual(functions['both'], 2)
    self.assertEqual(functions['ghidra_only'], 1)
    self.assertEqual(functions['ghidra_only_kind'], {'not_covered': 1})
    self.assertEqual(functions['ghidra_only_not_covered_by_step'], {'gaps': 1})
    self.assertEqual(functions['psp_only_where'], {'outside_ghidra_exec': 1})
    self.assertEqual(functions['psp_only_outside_ghidra_exec_by_section'],
                     {'.rodata': 1})
    coverage = summary['coverage_words']
    self.assertEqual(coverage['ghidra_code'], 10)
    self.assertEqual(coverage['ghidra_not_psp'], 3)
    self.assertEqual(coverage['psp_not_ghidra_by_section'], {'.rodata': 2})
    self.assertEqual(summary['jump_tables']['tables_fully_dispatchable'], 1)
    self.assertEqual(summary['decoder']['generic_in_ghidra_code'],
                     {'break': 1})
    self.assertEqual(summary['decoder']['control_flow_in_delay_slot'], 0)

  def test_delay_slot_control_flow(self):
    ghidra = recomp_compare.Ghidra(_ghidra(functions=[(_BASE, _BASE + 0x28)]))
    kinds = [2, 4] + [0] * 8  # jr with a jal in its delay slot
    psp = recomp_compare.Psp(_export(
        functions=[(_BASE, [(_BASE, _BASE + 0x8)])],
        seeds=[(_BASE, 'elf_entry')], kinds=kinds, entry_labels=[_BASE]))
    summary = recomp_compare.compare(ghidra, psp, {_BASE: 'ghidra'})
    self.assertEqual(summary['decoder']['control_flow_in_delay_slot'], 1)

  def test_bad_census(self):
    document = _export(functions=[], seeds=[], kinds=[0])
    document['census'][0]['kinds'] = 'abc'
    with self.assertRaises(recomp_compare.CompareError):
      recomp_compare.Psp(document)


@unittest.skipUnless(os.environ.get('MPO_ANALYSIS_EXPORT'),
                     'MPO_ANALYSIS_EXPORT not set (run through ctest)')
class EndToEndTest(unittest.TestCase):
  """mpo_analysis_export on a synthetic PRX, then recomp_compare."""

  def test_synthetic_prx(self):
    # PSPRecomp reads PRX relocations from the section table, which the
    # synthetic module does not have, so the jal is already absolute.
    text = [
        0x0C000000 | ((_BASE + 0x10) >> 2) & 0x03FFFFFF,  # 0x00: jal 0x10
        0x00000000,  # nop
        0x03E00008,  # jr ra
        0x00000000,  # nop
        0x0000000C,  # 0x10: syscall (still generic for the emitter)
        0x03E00008,  # jr ra
        0x00000000,  # nop
        0x24020001,  # 0x1C: nothing references this function
        0x03E00008,  # jr ra
        0x00000000,  # nop
    ]
    prx = synthetic_prx.build(text, data=b'\0' * 16)
    with tempfile.TemporaryDirectory() as tmp:
      tmp = pathlib.Path(tmp)
      (tmp / 'synthetic.prx').write_bytes(prx)
      result = subprocess.run(
          [os.environ['MPO_ANALYSIS_EXPORT'], str(tmp / 'synthetic.prx'),
           _h(_BASE), str(tmp / 'export.json')],
          capture_output=True, text=True, check=False)
      self.assertEqual(result.returncode, 0, result.stderr)
      export = json.loads((tmp / 'export.json').read_text())
      ghidra = _ghidra(functions=[(_BASE, _BASE + 0x10),
                                  (_BASE + 0x10, _BASE + 0x1C),
                                  (_BASE + 0x1C, _BASE + 0x28)])
      summary = recomp_compare.compare(recomp_compare.Ghidra(ghidra),
                                       recomp_compare.Psp(export),
                                       {_BASE: 'ghidra',
                                        _BASE + 0x10: 'ghidra',
                                        _BASE + 0x1C: 'gaps'})
      self.assertEqual(summary['functions']['both'], 2)
      self.assertEqual(summary['functions']['ghidra_only_not_covered_by_step'],
                       {'gaps': 1})
      self.assertEqual(summary['coverage_words']['ghidra_not_psp'], 3)
      self.assertEqual(summary['decoder']['generic_in_ghidra_code'],
                       {'syscall': 1})
      self.assertEqual(
          {s['name'] for s in export['sections']} & {'.text'}, {'.text'})
      # A hints file needs the framework patch; without it the tool says so.
      (tmp / 'hints.txt').write_text('seed 0x0880401C\n')
      result = subprocess.run(
          [os.environ['MPO_ANALYSIS_EXPORT'], str(tmp / 'synthetic.prx'),
           _h(_BASE), str(tmp / 'export2.json'), str(tmp / 'hints.txt')],
          capture_output=True, text=True, check=False)
      if result.returncode == 0:
        export = json.loads((tmp / 'export2.json').read_text())
        self.assertIn(_h(_BASE + 0x1C), [a for a, _ in export['seeds']])
      else:
        self.assertEqual(result.returncode, 2)
        self.assertIn('analysis hints', result.stderr)


if __name__ == '__main__':
  unittest.main()
