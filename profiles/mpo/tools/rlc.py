#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Parses, validates and applies a stage relocation table (`<stage>.rlc`).

A `.rlc` is an entry of the stage's `_zar` (see zar.py). It is an array of
8-byte entries with the same layout as the classic PRX relocations
(PT_PRXRELOC, 0x700000A0): u32 r_offset, u32 r_info, where r_info is
`type | ofs_base << 8 | addr_base << 16`. The difference is where the two
bases point:

  - ofs_base: segment of the *stage* PRX that holds the word to patch.
  - addr_base: segment of *mgp_main* whose load address is added.

So the table links the stage's references to mgp_main (calls, pointers and
data addresses), which the stage PRX does not import through NIDs. The
semantics of each type follow PPSSPP ElfReader::LoadRelocations (v1.20.4); see
docs/formats/rlc.md.

The input is untrusted: every entry is checked against the segment sizes before
anything is patched, and errors raise RlcError.

Usage:
  python -I profiles/mpo/tools/rlc.py check --stage <s>.prx \
      (--rlc <s>.rlc | --zar <_zar>) --main mgp_main.elf
  python -I profiles/mpo/tools/rlc.py relocate --stage <s>.prx --zar <_zar> \
      --main mgp_main.elf --stage-base 0x08B8F900 --output analysis/<s>.bin
  python -I profiles/mpo/tools/rlc.py link --stage <s>.prx --zar <_zar> \
      --main mgp_main.elf --output analysis/<s>.linked.prx
`check` prints only counts. `relocate` and `link` write game bytes: keep the
output in an ignored directory (analysis/) and never commit it.
"""

import argparse
import collections
import importlib.util
import pathlib
import struct
import sys

R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6

TYPE_NAMES = {
    R_MIPS_32: 'R_MIPS_32',
    R_MIPS_26: 'R_MIPS_26',
    R_MIPS_HI16: 'R_MIPS_HI16',
    R_MIPS_LO16: 'R_MIPS_LO16',
}
# Types seen in the 106 .rlc tables of ULUS-10202. Anything else is rejected
# instead of being guessed.
SUPPORTED_TYPES = frozenset(TYPE_NAMES)

_OP_J = 0x02
_OP_JAL = 0x03
_OP_LUI = 0x0F

ENTRY_SIZE = 8
# The largest .rlc on the disc has 7 125 entries; PSP RAM bounds the rest.
MAX_ENTRIES = 1 << 20

Entry = collections.namedtuple('Entry', 'offset type ofs_base addr_base')


class RlcError(Exception):
  """Raised when a relocation table is malformed or does not fit."""


def parse(data):
  """Returns the list of Entry in a .rlc table (structure checks only)."""
  if len(data) % ENTRY_SIZE:
    raise RlcError(f'size {len(data)} is not a multiple of {ENTRY_SIZE}')
  if len(data) // ENTRY_SIZE > MAX_ENTRIES:
    raise RlcError(f'{len(data) // ENTRY_SIZE} entries exceed the limit')
  entries = []
  for index in range(0, len(data), ENTRY_SIZE):
    offset, info = struct.unpack_from('<II', data, index)
    if info >> 24:
      raise RlcError(f'entry {index // ENTRY_SIZE}: r_info 0x{info:08X} has '
                     'unknown high bits')
    entry = Entry(offset=offset, type=info & 0xFF, ofs_base=(info >> 8) & 0xFF,
                  addr_base=(info >> 16) & 0xFF)
    if entry.type not in SUPPORTED_TYPES:
      raise RlcError(f'entry {index // ENTRY_SIZE}: unsupported type '
                     f'{entry.type}')
    if entry.offset & 3:
      raise RlcError(f'entry {index // ENTRY_SIZE}: offset 0x{offset:X} is '
                     'not word aligned')
    entries.append(entry)
  _check_hi16_pairs(entries)
  return entries


def from_prx_relocations(relocations):
  """Converts psp_prx.Relocation tuples to Entry, with the same checks."""
  return parse(b''.join(
      struct.pack('<II', r.offset, r.info) for r in relocations))


def _check_hi16_pairs(entries):
  # PPSSPP pairs a HI16 with the next entry that is not a HI16, which must be
  # a LO16 with the same bases (several HI16 may share one LO16).
  for index, entry in enumerate(entries):
    if entry.type != R_MIPS_HI16:
      continue
    partner = _lo16_partner(entries, index)
    if partner is None:
      raise RlcError(f'entry {index}: R_MIPS_HI16 without a following '
                     'R_MIPS_LO16')
    other = entries[partner]
    if (other.ofs_base, other.addr_base) != (entry.ofs_base, entry.addr_base):
      raise RlcError(f'entry {index}: R_MIPS_HI16/LO16 pair with different '
                     'bases')


def _lo16_partner(entries, index):
  for later in range(index + 1, len(entries)):
    if entries[later].type == R_MIPS_HI16:
      continue
    return later if entries[later].type == R_MIPS_LO16 else None
  return None


def check_bounds(entries, patch_sizes, target_sizes):
  """Checks every entry against segment sizes.

  Args:
    entries: list of Entry.
    patch_sizes: size in bytes of each segment of the patched module (the
      stage PRX), indexed by ofs_base.
    target_sizes: size in bytes of each segment of the target module
      (mgp_main), indexed by addr_base.
  """
  for index, entry in enumerate(entries):
    if entry.ofs_base >= len(patch_sizes):
      raise RlcError(f'entry {index}: ofs_base {entry.ofs_base} does not exist')
    if entry.offset + 4 > patch_sizes[entry.ofs_base]:
      raise RlcError(f'entry {index}: offset 0x{entry.offset:X} is outside '
                     f'segment {entry.ofs_base}')
    if entry.addr_base >= len(target_sizes):
      raise RlcError(f'entry {index}: addr_base {entry.addr_base} does not '
                     'exist')


def _hi_lo(address):
  """Splits an address into the lui/addiu halves (PPSSPP addrToHiLo)."""
  lo = address & 0xFFFF
  signed_lo = lo - 0x10000 if lo & 0x8000 else lo
  hi = ((address - signed_lo) >> 16) & 0xFFFF
  return hi, lo


def apply(memory, memory_base, entries, patch_bases, target_bases):
  """Applies `entries` in place, like PPSSPP ElfReader::LoadRelocations.

  Every word is read before any is written, so the result does not depend on
  the entry order (a LO16 is patched after its HI16 has used the original).

  Args:
    memory: bytearray holding the patched module (and anything else).
    memory_base: PSP address of memory[0].
    entries: list of Entry, already bounds-checked.
    patch_bases: PSP address of each segment of the patched module.
    target_bases: PSP address of each segment of the target module.
  """

  def location(entry):
    if entry.ofs_base >= len(patch_bases):
      raise RlcError(f'ofs_base {entry.ofs_base} has no load address')
    if entry.addr_base >= len(target_bases):
      raise RlcError(f'addr_base {entry.addr_base} has no load address')
    position = patch_bases[entry.ofs_base] + entry.offset - memory_base
    if position < 0 or position + 4 > len(memory):
      raise RlcError(f'patched word at 0x{position + memory_base:08X} is '
                     'outside memory')
    return position

  positions = [location(entry) for entry in entries]
  originals = [
      struct.unpack_from('<I', memory, position)[0] for position in positions
  ]
  for index, entry in enumerate(entries):
    op = originals[index]
    target = target_bases[entry.addr_base]
    if entry.type == R_MIPS_32:
      op = (op + target) & 0xFFFFFFFF
    elif entry.type == R_MIPS_26:
      field = ((op & 0x03FFFFFF) + (target >> 2)) & 0x03FFFFFF
      op = (op & 0xFC000000) | field
    elif entry.type == R_MIPS_HI16:
      partner = _lo16_partner(entries, index)
      if partner is None:
        raise RlcError(f'entry {index}: R_MIPS_HI16 without R_MIPS_LO16')
      lo = originals[partner] & 0xFFFF
      lo = lo - 0x10000 if lo & 0x8000 else lo
      address = (((op & 0xFFFF) << 16) + lo + target) & 0xFFFFFFFF
      op = (op & 0xFFFF0000) | _hi_lo(address)[0]
    elif entry.type == R_MIPS_LO16:
      op = (op & 0xFFFF0000) | (((op & 0xFFFF) + target) & 0xFFFF)
    struct.pack_into('<I', memory, positions[index], op)


def resolve(entries, words, target_sizes):
  """Returns, per entry, the target-module-relative address it refers to.

  `words` are the original (unpatched) words of each entry, in order. Used to
  check that every reference lands inside the target segment.
  """
  resolved = []
  for index, entry in enumerate(entries):
    op = words[index]
    if entry.type == R_MIPS_32:
      value = op
    elif entry.type == R_MIPS_26:
      value = (op & 0x03FFFFFF) << 2
    elif entry.type == R_MIPS_HI16:
      lo = words[_lo16_partner(entries, index)] & 0xFFFF
      lo = lo - 0x10000 if lo & 0x8000 else lo
      value = ((op & 0xFFFF) << 16) + lo
    else:
      value = None  # A LO16 alone is resolved through its HI16.
    if value is not None and not 0 <= value < target_sizes[entry.addr_base]:
      raise RlcError(f'entry {index}: reference 0x{value:X} is outside '
                     f'target segment {entry.addr_base}')
    resolved.append(value)
  return resolved


# -- Command line --------------------------------------------------------------


def _load_sibling(name):
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def _segment_sizes(prx):
  return [segment.memsz for segment in prx.load_segments]


def check(stage, table, main):
  """Validates `table` against a stage and mgp_main; returns report lines."""
  entries = parse(table)
  check_bounds(entries, _segment_sizes(stage), _segment_sizes(main))
  image = stage.image()
  words = [
      struct.unpack_from(
          '<I', image, stage.load_segments[e.ofs_base].vaddr + e.offset)[0]
      for e in entries
  ]
  # A table from another stage usually fits the segments but not the code.
  for index, (entry, word) in enumerate(zip(entries, words)):
    if entry.type == R_MIPS_26 and word >> 26 not in (_OP_J, _OP_JAL):
      raise RlcError(f'entry {index}: R_MIPS_26 does not patch a j/jal')
    if entry.type == R_MIPS_HI16 and word >> 26 != _OP_LUI:
      raise RlcError(f'entry {index}: R_MIPS_HI16 does not patch a lui')
  resolved = resolve(entries, words, _segment_sizes(main))
  own = from_prx_relocations(stage.relocations)
  own_locations = {(e.ofs_base, e.offset) for e in own}
  overlap = sum((e.ofs_base, e.offset) in own_locations for e in entries)
  kinds = collections.Counter(
      (TYPE_NAMES[e.type], e.ofs_base, e.addr_base) for e in entries)
  calls = {v for e, v in zip(entries, resolved) if e.type == R_MIPS_26}
  lines = [
      f'entries        {len(entries)}',
      f'own PRX relocs {len(own)} (shared locations with .rlc: {overlap})',
      f'call targets   {len(calls)} distinct mgp_main addresses',
  ]
  for (name, ofs_base, addr_base), count in sorted(kinds.items()):
    lines.append(f'  {name:12} stage seg {ofs_base} -> mgp_main seg '
                 f'{addr_base}: {count}')
  if overlap:
    raise RlcError(f'{overlap} entries patch words the PRX relocates itself')
  return lines


def relocate(stage, table, main, stage_base, main_base):
  """Returns the stage image as the game leaves it, loaded at `stage_base`.

  First the PRX's own relocations (what sceKernelLoadModule does), then the
  `.rlc` against mgp_main loaded at `main_base` (what the game does). The two
  sets never patch the same word (check() verifies it), so the order does not
  change the result.
  """
  for base, what in ((stage_base, 'stage'), (main_base, 'mgp_main')):
    if base & 0xFF:
      raise RlcError(f'{what} base 0x{base:08X} is not 256-byte aligned')
  check(stage, table, main)
  entries = parse(table)
  own = from_prx_relocations(stage.relocations)
  image = stage.image()
  stage_bases = [stage_base + s.vaddr for s in stage.load_segments]
  main_bases = [main_base + s.vaddr for s in main.load_segments]
  check_bounds(own, _segment_sizes(stage), _segment_sizes(stage))
  apply(image, stage_base, own, stage_bases, stage_bases)
  apply(image, stage_base, entries, stage_bases, main_bases)
  return image


def link(stage, table, main, main_base):
  """Returns the stage PRX file with the `.rlc` already applied.

  The words the `.rlc` patches get their final values for mgp_main at
  `main_base`; the PRX's own relocations are left untouched. Loading the
  result at any base (sceKernelLoadModule, Ghidra's PSP loader or a
  recompiler) then gives the same image the game builds at run time, without
  running the game's linker (mgp_main 0x08805C0C).
  """
  if main_base & 0xFF:
    raise RlcError(f'mgp_main base 0x{main_base:08X} is not 256-byte aligned')
  check(stage, table, main)
  entries = parse(table)
  check_bounds(entries, [s.filesz for s in stage.load_segments],
               _segment_sizes(main))
  data = bytearray(stage.data)
  file_bases = [s.offset for s in stage.load_segments]
  main_bases = [main_base + s.vaddr for s in main.load_segments]
  apply(data, 0, entries, file_bases, main_bases)
  return bytes(data)


def _read_table(args, stage_path, zar):
  if args.rlc:
    with open(args.rlc, 'rb') as rlc_file:
      return rlc_file.read()
  with open(args.zar, 'rb') as zar_file:
    dar = zar.decompress(zar_file.read())
  name = pathlib.Path(stage_path).stem + '.rlc'
  return zar.read_entry(dar, zar.parse_dar(dar), name)


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  check_parser = commands.add_parser('check', help='validate a .rlc')
  relocate_parser = commands.add_parser(
      'relocate', help='write the relocated stage image (for local '
      'comparison against a PPSSPP memory dump; never commit it)')
  link_parser = commands.add_parser(
      'link', help='write the stage PRX with the .rlc pre-applied (game '
      'bytes: keep it in analysis/)')
  for sub in (check_parser, relocate_parser, link_parser):
    sub.add_argument('--stage', required=True, help='stage PRX')
    source = sub.add_mutually_exclusive_group(required=True)
    source.add_argument('--rlc', help='extracted .rlc table')
    source.add_argument('--zar', help="the stage's _zar (reads <stage>.rlc)")
    sub.add_argument('--main', required=True, help='mgp_main ELF')
  relocate_parser.add_argument('--stage-base', required=True,
                               type=lambda text: int(text, 0))
  for sub in (relocate_parser, link_parser):
    sub.add_argument('--main-base', default=0x08804000,
                     type=lambda text: int(text, 0))
    sub.add_argument('--output', required=True)
  args = parser.parse_args(argv)

  psp_prx = _load_sibling('psp_prx')
  zar = _load_sibling('zar')
  try:
    with open(args.stage, 'rb') as stage_file:
      stage = psp_prx.Prx(stage_file.read())
    with open(args.main, 'rb') as main_file:
      main_module = psp_prx.Prx(main_file.read())
    table = _read_table(args, args.stage, zar)
    if args.command == 'check':
      print('\n'.join(check(stage, table, main_module)))
      return 0
    if args.command == 'link':
      image = link(stage, table, main_module, args.main_base)
    else:
      image = relocate(stage, table, main_module, args.stage_base,
                       args.main_base)
  except (RlcError, psp_prx.PrxError, zar.ZarError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 1
  with open(args.output, 'wb') as output_file:
    output_file.write(image)
  print(f'wrote {len(image)} bytes')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
