#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inventory of the stage overlays (`stage/<s>/<s>.prx` + `<s>.rlc`).

For every stage it checks the PRX and _zar against supported.toml, parses the
PRX (segments, sceModuleInfo, imports, exports, own relocations) and the
`.rlc` inside its _zar, and validates the `.rlc` against mgp_main. Then it
measures how much code the stages share: each `.text` is cut into functions
(starts taken from the relocations), the words patched by relocations are
masked so the code no longer depends on where it is loaded, and the functions
are hashed.

Only numbers and names are printed (Markdown), so the output can go to docs/.

Usage:
  python -I profiles/mpo/tools/stage_inventory.py \
      --stages analysis/f04/game/disc/PSP_GAME/USRDIR/stage \
      --main game/elf/mgp_main.elf \
      --supported profiles/mpo/config/supported.toml
The stage directories must hold both `<s>.prx` and `_zar` (mpo --install only
extracts `*.prx`; see docs/formats/rlc.md for how to get `_zar`).
"""

import argparse
import collections
import hashlib
import importlib.util
import pathlib
import struct
import sys


def _load_sibling(name):
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


psp_prx = _load_sibling('psp_prx')
rlc = _load_sibling('rlc')
zar = _load_sibling('zar')

# addiu, lb, lh, lw, lbu, lhu, sb, sh, sw, lwc1, swc1, lv.s, sv.s, lv.q, sv.q.
_GP_BASE_OPCODES = frozenset(
    (0x09, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x39, 0x32,
     0x3A, 0x36, 0x3E))

Stage = collections.namedtuple(
    'Stage', 'name prx_size prx_sha256 zar_size prx entries hashes_ok')


def _sha256(data):
  return hashlib.sha256(data).hexdigest()


def _supported_hashes(path):
  # Python 3.11+; imported here so the tests also run on 3.10.
  import tomllib  # pylint: disable=import-outside-toplevel
  with open(path, 'rb') as toml_file:
    config = tomllib.load(toml_file)
  hashes = {}
  for version in config['version']:
    for entry in version.get('file', []):
      hashes[entry['path']] = entry['sha256']
  return hashes


def _load_stage(directory, main, hashes):
  name = directory.name
  prx_bytes = (directory / f'{name}.prx').read_bytes()
  zar_bytes = (directory / '_zar').read_bytes()
  prefix = f'PSP_GAME/USRDIR/stage/{name}/'
  hashes_ok = (hashes.get(prefix + f'{name}.prx') == _sha256(prx_bytes) and
               hashes.get(prefix + '_zar') == _sha256(zar_bytes))
  prx = psp_prx.Prx(prx_bytes)
  dar = zar.decompress(zar_bytes)
  table = zar.read_entry(dar, zar.parse_dar(dar), f'{name}.rlc')
  rlc.check(prx, table, main)  # Raises if the table does not fit.
  return Stage(name=name, prx_size=len(prx_bytes),
               prx_sha256=_sha256(prx_bytes), zar_size=len(zar_bytes), prx=prx,
               entries=rlc.parse(table), hashes_ok=hashes_ok)


# -- Position-independent function hashing -------------------------------------


def _relocation_kinds(prx, extra_entries=()):
  """Yields (ofs_base, offset, type, addr_base) for own and extra relocs."""
  for relocation in prx.relocations:
    info = relocation.info
    yield ((info >> 8) & 0xFF, relocation.offset, info & 0xFF,
           (info >> 16) & 0xFF)
  for entry in extra_entries:
    yield (entry.ofs_base, entry.offset, entry.type, -1)  # -1: mgp_main.


def functions(prx, extra_entries=()):
  """Returns [(start, size, digest)] for the functions in `.text`.

  Starts are the text start plus every in-module call target and code pointer
  found in the relocations; words patched by any relocation are masked.
  """
  text = prx.section('.text')
  if text is None or text.size == 0:
    return []
  image = prx.image()
  if text.addr + text.size > len(image):
    raise psp_prx.PrxError('.text is outside the module image')
  words = list(struct.unpack_from(f'<{text.size // 4}I', image, text.addr))
  original = list(words)
  starts = {text.addr}
  relocations = list(_relocation_kinds(prx, extra_entries))
  for ofs_base, offset, kind, addr_base in relocations:
    if ofs_base >= len(prx.load_segments):
      continue
    location = prx.load_segments[ofs_base].vaddr + offset
    if location + 4 > len(image):
      continue
    word = struct.unpack_from('<I', image, location)[0]
    in_module = addr_base >= 0 and addr_base < len(prx.load_segments)
    if in_module and kind == rlc.R_MIPS_26:
      starts.add(((word & 0x03FFFFFF) << 2) +
                 prx.load_segments[addr_base].vaddr)
    elif in_module and kind == rlc.R_MIPS_32:
      starts.add(prx.load_segments[addr_base].vaddr + word)
    index = (location - text.addr) // 4
    if text.addr <= location < text.addr + text.size and not location & 3:
      if kind == rlc.R_MIPS_26:
        words[index] = original[index] & 0xFC000000
      elif kind in (rlc.R_MIPS_HI16, rlc.R_MIPS_LO16):
        words[index] = original[index] & 0xFFFF0000
      elif kind == rlc.R_MIPS_32:
        words[index] = 0
  ordered = sorted(s for s in starts
                   if text.addr <= s < text.addr + text.size and not s & 3)
  result = []
  for position, start in enumerate(ordered):
    end = ordered[position + 1] if position + 1 < len(ordered) else (
        text.addr + text.size)
    first, last = (start - text.addr) // 4, (end - text.addr) // 4
    digest = hashlib.sha1(struct.pack(f'<{last - first}I',
                                      *words[first:last])).digest()
    result.append((start, end - start, digest))
  return result


def gp_relative_count(prx):
  """Counts loads, stores and addiu in `.text` that use $gp as base."""
  text = prx.section('.text')
  if text is None:
    return 0
  image = prx.image()
  count = 0
  for word in struct.unpack_from(f'<{text.size // 4}I', image, text.addr):
    if word >> 26 in _GP_BASE_OPCODES and (word >> 21) & 31 == 28:
      count += 1
  return count


def image_size(prx):
  """Bytes the module takes in memory, rounded like the PSP (0x100)."""
  end = max(s.vaddr + s.memsz for s in prx.load_segments)
  return (end + 0xFF) & ~0xFF


# -- Report --------------------------------------------------------------------


def _family(name):
  if name.startswith('st') and name[2:4].isdigit():
    return 'st (misiones)'
  if name.startswith('n') and name[1:3].isdigit():
    return 'n (versus/red)'
  if name.startswith('r_') or name == 'init':
    return 'r_ e init (residentes)'
  return 'menús y demos'


def _import_summary(prx):
  parts = []
  for library in prx.imports:
    count = str(len(library.function_nids))
    if library.variable_nids:
      count += f'+{len(library.variable_nids)}'
    parts.append(f'{library.name} ({count})')
  return ', '.join(parts) or '—'


def _export_summary(prx):
  parts = []
  for library in prx.exports:
    name = library.name or 'syslib'
    nids = library.function_nids + library.variable_nids
    labels = [psp_prx.SYSLIB_NIDS.get(nid, f'0x{nid:08X}') for nid in nids]
    parts.append(f'{name}: {" ".join(labels)}')
  return '; '.join(parts) or '—'


def report(stages, main):
  lines = []
  distinct = {}
  for stage in stages:
    distinct.setdefault(stage.prx_sha256, stage)
  lines += [
      '## Resumen', '',
      f'- Etapas: {len(stages)}; PRX distintos (por SHA-256): {len(distinct)}.',
      f'- Hashes de `<s>.prx` y `_zar` iguales a `supported.toml`: '
      f'{sum(s.hashes_ok for s in stages)} de {len(stages)}.',
      f'- Nombre de módulo: '
      f'{", ".join(sorted({s.prx.module_info.name for s in stages}))}.',
      f'- Bytes de PRX: {sum(s.prx_size for s in stages)} en total; '
      f'{sum(s.prx_size for s in distinct.values())} sin repetir.',
      f'- Entradas `.rlc`: {sum(len(s.entries) for s in stages)} en total; '
      f'relocaciones propias (PT_PRXRELOC): '
      f'{sum(len(s.prx.relocations) for s in stages)}.',
  ]
  largest = max(stages, key=lambda s: image_size(s.prx))
  lines.append(f'- Imagen en memoria más grande: `{largest.name}`, '
               f'0x{image_size(largest.prx):X} bytes (con `.bss`, redondeada a '
               '0x100).')
  identical = [
      sorted(s.name for s in stages if s.prx_sha256 == digest)
      for digest in distinct
  ]
  groups = [g for g in identical if len(g) > 1]
  lines.append(f'- Grupos de etapas con el mismo PRX: {len(groups)} '
               f'({"; ".join(" = ".join(g) for g in groups)}).')

  # Imports/exports patterns.
  import_patterns = collections.Counter(_import_summary(s.prx) for s in stages)
  export_patterns = collections.Counter(_export_summary(s.prx) for s in stages)
  lines += ['', '### Imports y exports', '']
  for pattern, count in import_patterns.most_common():
    lines.append(f'- Imports `{pattern}`: {count} etapas.')
  for pattern, count in export_patterns.most_common():
    lines.append(f'- Exports `{pattern}`: {count} etapas.')

  # Entry points into mgp_main.
  main_calls = set()
  main_data = collections.Counter()
  main_image = main.image()
  for stage in distinct.values():
    image = stage.prx.image()
    for entry in stage.entries:
      location = stage.prx.load_segments[entry.ofs_base].vaddr + entry.offset
      word = struct.unpack_from('<I', image, location)[0]
      if entry.type == rlc.R_MIPS_26:
        main_calls.add((word & 0x03FFFFFF) << 2)
      else:
        main_data[(rlc.TYPE_NAMES[entry.type], entry.addr_base)] += 1
  internal_calls = set()
  for relocation in main.relocations:
    if relocation.info & 0xFF == rlc.R_MIPS_26:
      location = main.load_segments[(relocation.info >> 8) & 0xFF].vaddr + (
          relocation.offset)
      word = struct.unpack_from('<I', main_image, location)[0]
      internal_calls.add((word & 0x03FFFFFF) << 2)
  lines += [
      '', '### Referencias a `mgp_main` (de los PRX distintos)', '',
      f'- Destinos de `jal`/`j` en `mgp_main`: {len(main_calls)} direcciones '
      f'distintas; {len(main_calls & internal_calls)} también son destino de '
      f'algún `jal`/`j` de `mgp_main` y {len(main_calls - internal_calls)} '
      'no lo son (semillas de funciones para el análisis de `mgp_main`).',
  ]
  for (name, addr_base), count in sorted(main_data.items()):
    lines.append(f'- {name} hacia el segmento {addr_base} de `mgp_main`: '
                 f'{count} entradas.')

  # Shared code.
  per_stage = {}
  owners = collections.defaultdict(set)
  sizes = {}
  for digest, stage in distinct.items():
    funcs = functions(stage.prx, stage.entries)
    per_stage[digest] = funcs
    for _, size, key in funcs:
      owners[key].add(digest)
      sizes[key] = size
  main_keys = {key for _, _, key in functions(main)}
  total = sum(size for funcs in per_stage.values() for _, size, _ in funcs)
  unique = sum(sizes.values())
  main_text = main.section('.text')
  gp_stage = sum(gp_relative_count(s.prx) for s in distinct.values())
  lines += [
      '', '### Código compartido entre etapas (`.text` de los PRX distintos)',
      '',
      f'- `.text` total: {total} bytes en '
      f'{sum(len(f) for f in per_stage.values())} funciones '
      f'({total / max(main_text.size if main_text else 1, 1):.1f} veces el '
      '`.text` de `mgp_main`).',
      f'- Funciones distintas: {len(sizes)}, que suman {unique} bytes '
      f'({100 * unique / total:.1f} % del total).',
      f'- Funciones idénticas a una de `mgp_main`: '
      f'{sum(1 for k in sizes if k in main_keys)} '
      f'({sum(sizes[k] for k in sizes if k in main_keys)} bytes).',
      f'- Instrucciones con base `$gp` en `.text`: {gp_stage} en las etapas y '
      f'{gp_relative_count(main)} en `mgp_main`.',
  ]
  lines += [
      '', '| Familia | Etapas | PRX distintos | `.text` (bytes) | '
      'Compartido con otra etapa | Exclusivo |',
      '|---|---:|---:|---:|---:|---:|'
  ]
  families = collections.defaultdict(list)
  for digest, stage in distinct.items():
    families[_family(stage.name)].append(digest)
  for family in sorted(families):
    digests = families[family]
    text_bytes = shared = 0
    for digest in digests:
      for _, size, key in per_stage[digest]:
        text_bytes += size
        if len(owners[key]) > 1:
          shared += size
    count = sum(1 for s in stages if _family(s.name) == family)
    lines.append(
        f'| {family} | {count} | {len(digests)} | {text_bytes} | '
        f'{100 * shared / max(text_bytes, 1):.1f} % | '
        f'{100 * (text_bytes - shared) / max(text_bytes, 1):.1f} % |')

  # Per-stage table.
  lines += [
      '', '## Etapas', '',
      '| Etapa | PRX | `.text` | seg 0 | seg 1 (file/mem) | imagen | gp | '
      'relocs PRX | `.rlc` | imports | compartido |',
      '|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|'
  ]
  for stage in stages:
    prx = stage.prx
    seg0 = prx.load_segments[0]
    seg1 = prx.load_segments[1] if len(prx.load_segments) > 1 else None
    text = prx.section('.text')
    funcs = per_stage[stage.prx_sha256]
    text_bytes = sum(size for _, size, _ in funcs)
    shared = sum(size for _, size, key in funcs if len(owners[key]) > 1)
    nids = sum(len(l.function_nids) + len(l.variable_nids) for l in prx.imports)
    seg1_text = f'0x{seg1.filesz:X}/0x{seg1.memsz:X}' if seg1 else '—'
    lines.append(
        f'| `{stage.name}` | {stage.prx_size} | '
        f'0x{text.size if text else 0:X} | '
        f'0x{seg0.memsz:X} | {seg1_text} | 0x{image_size(prx):X} | '
        f'0x{prx.module_info.gp:X} | '
        f'{len(prx.relocations)} | {len(stage.entries)} | '
        f'{len(prx.imports)} libs, {nids} NIDs | '
        f'{100 * shared / max(text_bytes, 1):.0f} % |')
  return lines


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--stages', required=True, type=pathlib.Path)
  parser.add_argument('--main', required=True, type=pathlib.Path)
  parser.add_argument('--supported', required=True, type=pathlib.Path)
  args = parser.parse_args(argv)
  hashes = _supported_hashes(args.supported)
  try:
    main_module = psp_prx.Prx(args.main.read_bytes())
    stages = [
        _load_stage(directory, main_module, hashes)
        for directory in sorted(args.stages.iterdir())
        if directory.is_dir()
    ]
  except (psp_prx.PrxError, rlc.RlcError, zar.ZarError, OSError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 1
  print('\n'.join(report(stages, main_module)))
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
