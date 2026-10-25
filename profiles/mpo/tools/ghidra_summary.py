#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarizes the JSON written by profiles/mpo/tools/ghidra/run_analysis.sh.

Prints Markdown tables with numbers and names only (blocks, coverage per
seeding step, instruction classes, imports per library, exports and anchors),
so the output can go to docs/analysis/. Game strings are left out unless
--strings is given, and that mode is for local use only.

Usage:
  python -I profiles/mpo/tools/ghidra_summary.py analysis/out/mgp_main
"""

import argparse
import collections
import json
import os
import sys

_STEPS = (('1-ghidra', 'Ghidra (auto-analysis)'),
          ('2-relocations', '+ relocations'),
          ('3-prologues', '+ prologues'),
          ('', '+ gaps (final)'))


def _load(path):
  with open(path, encoding='utf-8') as json_file:
    return json.load(json_file)


def _pct(value):
  return f'{value:.2f} %'


def _table(header, rows):
  lines = ['| ' + ' | '.join(header) + ' |',
           '|' + '|'.join('---' for _ in header) + '|']
  lines += ['| ' + ' | '.join(str(cell) for cell in row) + ' |'
            for row in rows]
  return lines


def coverage(out_dir, program):
  rows = []
  for step, label in _STEPS:
    path = os.path.join(out_dir, step, f'{program}.metrics.json')
    if not os.path.exists(path):
      continue
    metrics = _load(path)
    text = [c for c in metrics['coverage'] if c['block'] == '.text']
    if not text:
      continue
    text = text[0]
    rows.append((label, metrics['functions_total'], text['functions'],
                 text['instructions'], _pct(text['decoded_percent']),
                 _pct(text['decoded_percent_excluding_zero_padding']),
                 _pct(text['function_percent']),
                 text['undefined_nonzero_bytes'],
                 len(text['nonzero_undefined_runs'])))
  return ['### Cobertura de `.text` por paso', ''] + _table(
      ('Paso', 'Funciones (total)', 'Funciones en .text', 'Instrucciones',
       'Decodificado', 'Decodificado sin relleno', 'En funciones',
       'Bytes ≠0 sin decodificar', 'Huecos ≠0'), rows)


def blocks(metrics):
  rows = [(b['name'], b['start'], f'0x{b["size"]:X}', b['flags'])
          for b in metrics['blocks'] if b['start'] != '0x00000000']
  return ['### Bloques de memoria', ''] + _table(
      ('Bloque', 'Inicio', 'Tamaño', 'Flags'), rows)


def classes(metrics):
  c = metrics['classes']

  def get(key):
    return c.get(key, 0)

  likely = sum(v for k, v in c.items() if k.startswith('branch_likely.'))
  rows = [
      ('Instrucciones (todos los bloques ejecutables)',
       metrics['instructions']),
      ('Funciones', metrics['functions_total']),
      ('VFPU (total)', get('vfpu')),
      ('VFPU: loads/stores (`lv`/`sv`)', get('vfpu.load_store')),
      ('VFPU: `vpfxs` / `vpfxt` / `vpfxd`',
       f'{get("vfpu.prefix.vpfxs")} / {get("vfpu.prefix.vpfxt")} / '
       f'{get("vfpu.prefix.vpfxd")}'),
      ('VFPU: `viim`/`vfim`', get('vfpu.viim_vfim')),
      ('Funciones con VFPU', metrics['functions_with_vfpu']),
      ('FPU (COP1, con `lwc1`/`swc1`)', get('fpu')),
      ('Funciones con FPU', metrics['functions_with_fpu']),
      ('Branch-likely (total)', likely),
      ('  `beql`/`bnel`/`blezl`/`bgtzl`',
       get('branch_likely.beql_bnel_blezl_bgtzl')),
      ('  REGIMM (`bltzl`/`bgezl`/`bltzall`/`bgezall`)',
       get('branch_likely.regimm')),
      ('  `bc1fl`/`bc1tl`', get('branch_likely.bc1fl_bc1tl')),
      ('  `bvfl`/`bvtl`', get('branch_likely.bvfl_bvtl')),
      ('`jr ra`', get('jr.ra')),
      ('`jr` (otro registro), resueltos', get('jr.resolved')),
      ('`jr` (otro registro), sin resolver', get('jr.unresolved')),
      ('`jalr` resueltos', get('jalr.resolved')),
      ('`jalr` sin resolver (punteros a función)', get('jalr.unresolved')),
      ('`syscall`', get('syscall')),
      ('`break`', get('break')),
      ('`cache`', get('cache')),
      ('`sync`', get('sync')),
      ('`ll`/`sc`', get('ll_sc')),
      ('`lwl`/`lwr`/`swl`/`swr`', get('lwl_lwr_swl_swr')),
      ('Allegrex SPECIAL3 (`ext`/`ins`/`seb`/`seh`/`wsbh`/`bitrev`...)',
       get('allegrex.special3')),
      ('Allegrex SPECIAL2 (`halt`/`mfic`/`mtic`)', get('allegrex.special2')),
      ('Allegrex `movz`/`movn`', get('allegrex.movz_movn')),
      ('Allegrex `min`/`max`', get('allegrex.max_min')),
      ('Allegrex `clz`/`clo`', get('allegrex.clz_clo')),
      ('Allegrex `madd(u)`/`msub(u)`', get('allegrex.madd_msub')),
      ('COP0', get('cop0')),
  ]
  top = sorted(metrics['vfpu_mnemonics'].items(), key=lambda kv: -kv[1])[:20]
  return (['### Clases de instrucciones', ''] +
          _table(('Clase', 'Cantidad'), rows) +
          ['', '### VFPU: las 20 instrucciones más usadas', ''] +
          _table(('Instrucción', 'Cantidad'), top))


def imports(modules):
  rows = []
  missing = []
  totals = collections.Counter()
  for lib in modules['imports']:
    status = collections.Counter(f['ppsspp'] for f in lib['functions'])
    calls = sum(f['call_sites'] for f in lib['functions'])
    rows.append((f'`{lib["name"]}`', lib['version'], lib['attr'],
                 len(lib['functions']), status['implemented'],
                 status['unimplemented'], status['unknown'], calls))
    totals.update(status)
    totals['functions'] += len(lib['functions'])
    totals['calls'] += calls
    for f in lib['functions']:
      if f['ppsspp'] != 'implemented':
        missing.append((f'`{lib["name"]}`', f['nid'], f'`{f["name"]}`',
                        f['ppsspp'], f['match'], f['call_sites']))
  rows.append(('**Total**', '', '', totals['functions'],
               totals['implemented'], totals['unimplemented'],
               totals['unknown'], totals['calls']))
  out = ['### Imports por librería', ''] + _table(
      ('Librería', 'Versión', 'Attr', 'Funciones', 'PPSSPP: impl.',
       'PPSSPP: sin impl.', 'NID desconocido', 'Llamadas'), rows)
  sony_missing = [m for m in missing
                  if m[0].strip('`') not in ('kjfs', 'sound2', 'zlibdec')]
  if sony_missing:
    out += ['', '### Imports de Sony que PPSSPP no implementa o no conoce',
            ''] + _table(('Librería', 'NID', 'Nombre', 'PPSSPP', 'Match',
                          'Llamadas'), sony_missing)
  exports = []
  for lib in modules['exports']:
    for e in lib['entries']:
      exports.append((f'`{lib["name"]}`', e['kind'], e['nid'],
                      f'`{e["name"]}`' if e['name'] else '?', e['address']))
  out += ['', '### Exports', ''] + _table(
      ('Librería', 'Tipo', 'NID', 'Nombre', 'Dirección'), exports)
  return out


def anchors(data, show_strings):
  out = ['### Anclajes', '']
  rows = []
  for anchor in data['anchors']:
    for match in anchor['matches'] or [None]:
      if match is None:
        rows.append((anchor['label'], anchor['kind'], '—',
                     '(sin coincidencias)', ''))
        continue
      where = match.get('stub') or match.get('address')
      what = match.get('import') or ''
      if anchor['kind'] == 'string':
        what = repr(match['text']) if show_strings else '(cadena)'
      refs = [k.split(' ')[0] if not k.startswith(('data', '(no')) else k
              for k in match['referenced_by']]
      rows.append((anchor['label'], anchor['kind'], where, what,
                   ', '.join(refs[:8]) + (' …' if len(refs) > 8 else '')))
  return out + _table(
      ('Anclaje', 'Tipo', 'Dirección', 'Qué', 'Referenciado desde'), rows)


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('out_dir', help='analysis/out/<program>')
  parser.add_argument('--strings', action='store_true',
                      help='show matched game strings (local use only)')
  args = parser.parse_args(argv)
  program = os.path.basename(os.path.normpath(args.out_dir))
  metrics = _load(os.path.join(args.out_dir, f'{program}.metrics.json'))
  modules = _load(os.path.join(args.out_dir, f'{program}.modules.json'))
  lines = [f'## {program}', ''] + blocks(metrics) + [''] + coverage(
      args.out_dir, program) + [''] + classes(metrics) + [''] + imports(
          modules)
  anchors_path = os.path.join(args.out_dir, f'{program}.anchors.json')
  if os.path.exists(anchors_path):
    lines += [''] + anchors(_load(anchors_path), args.strings)
  print('\n'.join(lines))
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
