#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Widescreen spike: the x scale of the perspective projection in mgp_main.

The CWCheat widescreen code of ULUS-10202 (`0x201E9728`) writes a float in
`.rodata` at 0x089E9728 (the code's offset plus 0x08800000). It is 1.0 in the
disc's mgp_main, and the projection set-up (0x08845124) loads it once and
passes it to the matrix builder (0x08859DE0) both as the x scale and as the
far end of the depth range (docs/analysis/widescreen.md).

  check   prints the value, the SHA-256 of the module and every `lui`/`lwc1`
          pair in the code that loads it (only addresses and numbers).
  value   prints the value for each target aspect ratio (width / height):
          (480 / 272) / aspect.
  patch   writes a copy of the module with another value, for local
          experiments only (the copy is game data: never commit it).

Usage:
  python -I profiles/mpo/tools/widescreen.py check game/elf/mgp_main.elf
  python -I profiles/mpo/tools/widescreen.py value 16/9 21/9 2560/1080 32/9
  python -I profiles/mpo/tools/widescreen.py patch game/elf/mgp_main.elf \
      2560/1080 analysis/spike-widescreen/elf/wide.elf
"""

import argparse
import fractions
import hashlib
import struct
import sys

_IMAGE_BASE = 0x08804000  # Where PPSSPP and the runtime load mgp_main.
_SCALE_ADDRESS = 0x089E9728  # CWCheat 0x201E9728.
_ORIGINAL = 0x3F800000  # 1.0f.
_PSP_ASPECT = fractions.Fraction(480, 272)
_LOOKBACK = 16  # Words searched backwards for the `lui` of a `lwc1`.


class ModuleError(Exception):
  """Raised when the module is not the expected little-endian MIPS PRX."""


def _unpack(fmt, data, offset):
  size = struct.calcsize(fmt)
  if offset < 0 or offset + size > len(data):
    raise ModuleError(f'read of {size} bytes at 0x{offset:X} is out of bounds')
  return struct.unpack_from(fmt, data, offset)


def _first_segment(data):
  """Returns (file offset, size) of the first LOAD segment (code and rodata)."""
  if len(data) < 52 or data[:4] != b'\x7fELF' or data[4:6] != b'\x01\x01':
    raise ModuleError('not a 32-bit little-endian ELF')
  e_type, machine = _unpack('<HH', data, 16)
  if e_type != 0xFFA0 or machine != 8:
    raise ModuleError('not a relocatable PSP module (PRX)')
  phoff, = _unpack('<I', data, 28)
  phentsize, phnum = _unpack('<HH', data, 42)
  if phnum == 0 or phentsize < 32:
    raise ModuleError('no program headers')
  p_type, p_offset, p_vaddr, _, p_filesz = _unpack('<5I', data, phoff)
  if p_type != 1 or p_vaddr != 0:
    raise ModuleError('the first program header is not the code segment')
  if p_offset + p_filesz > len(data):
    raise ModuleError('the code segment is out of bounds')
  return p_offset, p_filesz


def _scale_offset(data):
  seg_offset, seg_size = _first_segment(data)
  relative = _SCALE_ADDRESS - _IMAGE_BASE
  if relative + 4 > seg_size:
    raise ModuleError('the scale address is outside the code segment')
  return seg_offset + relative


def _load_sites(data):
  """Yields the vaddr of each `lwc1 ft, lo(rs)` whose `lui rs, hi` is near.

  The module is not relocated: `lui` holds the high half of the address
  relative to the image base, as the PRX relocations expect.
  """
  seg_offset, seg_size = _first_segment(data)
  relative = _SCALE_ADDRESS - _IMAGE_BASE
  lo = relative & 0xFFFF
  hi = (relative >> 16) + (1 if lo >= 0x8000 else 0)
  words = seg_size // 4
  for index in range(words):
    word, = _unpack('<I', data, seg_offset + index * 4)
    if word >> 26 != 0x31 or word & 0xFFFF != lo:
      continue
    base = (word >> 21) & 31
    for back in range(1, min(_LOOKBACK, index) + 1):
      prev, = _unpack('<I', data, seg_offset + (index - back) * 4)
      if prev >> 26 == 0x0F and (prev >> 16) & 31 == base and (
          prev & 0xFFFF) == hi:
        yield _IMAGE_BASE + index * 4, (word >> 16) & 31
        break


def _parse_aspect(text):
  try:
    aspect = fractions.Fraction(text)
  except (ValueError, ZeroDivisionError) as error:
    raise argparse.ArgumentTypeError(f'bad aspect ratio {text!r}') from error
  if aspect <= 0:
    raise argparse.ArgumentTypeError(f'bad aspect ratio {text!r}')
  return aspect


def _scale_for(aspect):
  value = float(_PSP_ASPECT / aspect)
  bits, = struct.unpack('<I', struct.pack('<f', value))
  return value, bits


def _check(args):
  with open(args.module, 'rb') as module_file:
    data = module_file.read()
  offset = _scale_offset(data)
  bits, = _unpack('<I', data, offset)
  value, = struct.unpack('<f', struct.pack('<I', bits))
  print(f'sha256         {hashlib.sha256(data).hexdigest()}')
  print(f'0x{_SCALE_ADDRESS:08X}     0x{bits:08X} ({value!r})'
        f'{"" if bits == _ORIGINAL else "  (not the original 1.0)"}')
  sites = list(_load_sites(data))
  for vaddr, ft in sites:
    print(f'  loaded by    lwc1 $f{ft} at 0x{vaddr:08X}')
  print(f'load sites     {len(sites)}')
  return 0


def _value(args):
  for aspect in args.aspects:
    value, bits = _scale_for(aspect)
    print(f'{str(aspect):>10} = {float(aspect):.4f}:1  ->  {value:.6f} '
          f'(0x{bits:08X})')
  return 0


def _patch(args):
  with open(args.module, 'rb') as module_file:
    data = bytearray(module_file.read())
  offset = _scale_offset(data)
  if _unpack('<I', data, offset)[0] != _ORIGINAL:
    raise ModuleError('the scale is not 1.0: not the disc module')
  value, bits = _scale_for(args.aspect)
  struct.pack_into('<I', data, offset, bits)
  with open(args.output, 'wb') as output_file:
    output_file.write(data)
  print(f'{args.output}: 0x{_SCALE_ADDRESS:08X} = {value:.6f} (0x{bits:08X})')
  return 0


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  commands = parser.add_subparsers(dest='command', required=True)
  check = commands.add_parser('check')
  check.add_argument('module', help='decrypted mgp_main.elf')
  check.set_defaults(run=_check)
  value = commands.add_parser('value')
  value.add_argument('aspects', nargs='+', type=_parse_aspect,
                     help='width/height, e.g. 21/9 or 2560/1080')
  value.set_defaults(run=_value)
  patch = commands.add_parser('patch')
  patch.add_argument('module', help='decrypted mgp_main.elf')
  patch.add_argument('aspect', type=_parse_aspect)
  patch.add_argument('output', help='copy to write (local, never committed)')
  patch.set_defaults(run=_patch)
  args = parser.parse_args(argv)
  try:
    return args.run(args)
  except (ModuleError, OSError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 1


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
