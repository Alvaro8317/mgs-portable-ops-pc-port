#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""External difftest executor that runs cases on Unicorn (MIPS32 24Kf).

Unicorn is GPL-2.0-only, so it is never linked into the project: mpo_difftest
runs this script as a separate process from a venv outside the repo, e.g.
~/tools/venv-unicorn (see docs/sessions/F07a.md). The protocol is described in
profiles/mpo/tests/difftest/external_executor.cpp.

Usage:
  python -I unicorn_exec.py CASES_FILE RESULTS_FILE

Exits with 77 when the unicorn module is not installed, so ctest reports the
test as skipped.

Unicorn is a plain MIPS32 core, not an Allegrex. Cases where the two are known
to differ, or where MIPS32 leaves the result unpredictable, are answered with
"unsupported" and left out of the comparison:
  - Allegrex-only encodings with no MIPS32 equivalent (max, min, wsbw, bitrev).
  clz/clo and madd/maddu/msub/msubu use other encodings on the Allegrex and
  are translated to their MIPS32 (SPECIAL2) form.
  - add/addi/sub when they overflow (MIPS32 traps; the Allegrex in PPSSPP
  does not).
  - div/divu by zero and INT_MIN / -1 (unpredictable in MIPS32).
  - FPU float-to-int conversions of NaN, infinities and out-of-range values
  (MIPS32 returns 2^31-1 for all of them; PPSSPP does not), and round.w.s
  when PPSSPP's floor(x + 0.5f) differs from round-half-even.
  - cfc1/ctc1 (QEMU keeps FCSR bits that the Allegrex interpreter drops).
"""

import math
import struct
import sys

try:
  import unicorn
  from unicorn import mips_const
except ImportError:
  print('unicorn_exec.py: the unicorn module is not installed', file=sys.stderr)
  sys.exit(77)

CODE_BASE = 0x08804000
MEM_BASE = 0x08900000
MEM_SIZE = 512
PAGE = 0x1000

INT_MIN = -(1 << 31)
INT_MAX = (1 << 31) - 1


def f32(bits):
  """Returns the float value of 32 bits."""
  return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def to_f32(value):
  """Rounds a Python float to the nearest float32 value."""
  return struct.unpack('<f', struct.pack('<f', value))[0]


def s32(value):
  """Interprets the low 32 bits as a signed integer."""
  value &= 0xFFFFFFFF
  return value - (1 << 32) if value & 0x80000000 else value


def parse_cases(path):
  """Parses the cases file into a list of dicts."""
  cases = []
  with open(path, encoding='ascii') as stream:
    tokens = stream.read().split()
  pos = 0

  def take(count):
    nonlocal pos
    out = tokens[pos:pos + count]
    pos += count
    return out

  while pos < len(tokens):
    tag, case_id, spec, num_ops = take(4)
    if tag != 'case':
      raise ValueError('expected "case", got ' + tag)
    case = {'id': int(case_id), 'spec': spec,
        'ops': [int(op, 16) for op in take(int(num_ops))]}
    for name, count in (('gpr', 32), ('hilo', 2), ('fpr', 32), ('fcr31', 1),
              ('v', 128), ('vctrl', 16)):
      words = take(count + 1)
      if words[0] != name:
        raise ValueError('expected ' + name)
      case[name] = [int(word, 16) for word in words[1:]]
    mem_tag, _, mem_hex = take(3)
    if mem_tag != 'mem':
      raise ValueError('expected "mem"')
    case['mem'] = bytes.fromhex(mem_hex)
    if take(1)[0] != 'end':
      raise ValueError('expected "end"')
    cases.append(case)
  return cases


def round_half_even_ok(value):
  """True if PPSSPP's round.w.s (floor(x + 0.5f)) equals round-half-even."""
  ppsspp = math.floor(to_f32(value + 0.5))
  return ppsspp == round(value)


def translate(case):
  """Returns the MIPS32 word for the case, or None if it must be skipped."""
  op = case['ops'][0]
  spec = case['spec']
  gpr = case['gpr']
  rs = (op >> 21) & 31
  rt = (op >> 16) & 31
  rd = (op >> 11) & 31
  if len(case['ops']) != 1 or spec in ('max', 'min', 'wsbw', 'bitrev', 'cfc1', 'ctc1'):
    return None
  if spec in ('clz', 'clo'):
    funct = 0x20 if spec == 'clz' else 0x21
    return 0x70000000 | (rs << 21) | (rd << 16) | (rd << 11) | funct
  if spec in ('madd', 'maddu', 'msub', 'msubu'):
    funct = {'madd': 0, 'maddu': 1, 'msub': 4, 'msubu': 5}[spec]
    return 0x70000000 | (rs << 21) | (rt << 16) | funct
  if spec in ('add', 'sub', 'addi'):
    a = s32(gpr[rs])
    if spec == 'addi':
      b = op & 0xFFFF
      if b >= 0x8000:
        b -= 0x10000
      result = a + b
    elif spec == 'add':
      result = a + s32(gpr[rt])
    else:
      result = a - s32(gpr[rt])
    if not INT_MIN <= result <= INT_MAX:
      return None
  if spec in ('div', 'divu'):
    if gpr[rt] == 0 or (spec == 'div' and s32(gpr[rs]) == INT_MIN and s32(gpr[rt]) == -1):
      return None
  if spec in ('round.w.s', 'trunc.w.s', 'ceil.w.s', 'floor.w.s', 'cvt.w.s'):
    fs = (op >> 11) & 31
    bits = case['fpr'][fs]
    value = f32(bits)
    if math.isnan(value) or math.isinf(value):
      return None
    if not -2147483648.0 <= value < 2147483648.0:
      return None
    if spec == 'round.w.s' and not round_half_even_ok(value):
      return None
    if spec == 'round.w.s' and not -2147483648.0 <= to_f32(value + 0.5) < 2147483648.0:
      return None
  return op


def run_case(emu, case, word):
  """Runs one translated instruction and returns the final state dict."""
  emu.mem_write(CODE_BASE, struct.pack('<I', word))
  emu.mem_write(MEM_BASE, case['mem'])
  for i in range(1, 32):
    emu.reg_write(mips_const.UC_MIPS_REG_0 + i, case['gpr'][i])
  emu.reg_write(mips_const.UC_MIPS_REG_HI, case['hilo'][0])
  emu.reg_write(mips_const.UC_MIPS_REG_LO, case['hilo'][1])
  for i in range(32):
    emu.reg_write(mips_const.UC_MIPS_REG_F0 + i, case['fpr'][i])
  fcsr = case['fcr31'][0] & (1 << 23)
  if case['spec'] == 'cvt.w.s':
    fcsr |= case['fcr31'][0] & 3  # Rounding mode; arithmetic always rounds to nearest.
  emu.reg_write(mips_const.UC_MIPS_REG_FCSR, fcsr)
  emu.emu_start(CODE_BASE, CODE_BASE + 4, count=1)
  out = dict(case)
  out['gpr'] = [0] + [emu.reg_read(mips_const.UC_MIPS_REG_0 + i) & 0xFFFFFFFF
            for i in range(1, 32)]
  out['hilo'] = [emu.reg_read(mips_const.UC_MIPS_REG_HI) & 0xFFFFFFFF,
                   emu.reg_read(mips_const.UC_MIPS_REG_LO) & 0xFFFFFFFF]
  out['fpr'] = [emu.reg_read(mips_const.UC_MIPS_REG_F0 + i) & 0xFFFFFFFF for i in range(32)]
  out['fcr31'] = [(case['fcr31'][0] & ~(1 << 23))
          | (emu.reg_read(mips_const.UC_MIPS_REG_FCSR) & (1 << 23))]
  out['mem'] = bytes(emu.mem_read(MEM_BASE, MEM_SIZE))
  return out


def make_emulator():
  """Creates a little-endian MIPS32 24Kf with the code and data pages mapped."""
  emu = unicorn.Uc(unicorn.UC_ARCH_MIPS, unicorn.UC_MODE_MIPS32 | unicorn.UC_MODE_LITTLE_ENDIAN)
  emu.ctl_set_cpu_model(mips_const.UC_CPU_MIPS32_24KF)
  emu.mem_map(CODE_BASE, PAGE)
  emu.mem_map(MEM_BASE, PAGE)
  # Status.CU1: enable the FPU.
  status = emu.reg_read(mips_const.UC_MIPS_REG_CP0_STATUS)
  emu.reg_write(mips_const.UC_MIPS_REG_CP0_STATUS, status | (1 << 29))
  return emu


def write_words(stream, tag, words):
  stream.write(tag + ' ' + ' '.join('%x' % (word & 0xFFFFFFFF) for word in words) + '\n')


def main(argv):
  if len(argv) != 3:
    print(__doc__, file=sys.stderr)
    return 2
  cases = parse_cases(argv[1])
  emu = make_emulator()
  with open(argv[2], 'w', encoding='ascii') as stream:
    for case in cases:
      word = translate(case)
      if word is None:
        stream.write('result %d unsupported\nend\n' % case['id'])
        continue
      try:
        out = run_case(emu, case, word)
      except unicorn.UcError as error:
        print('unicorn_exec.py: case %d (%s): %s' % (case['id'], case['spec'], error),
                      file=sys.stderr)
        stream.write('result %d unsupported\nend\n' % case['id'])
        continue
      stream.write('result %d ok\n' % case['id'])
      for name in ('gpr', 'hilo', 'fpr', 'fcr31', 'v', 'vctrl'):
        write_words(stream, name, out[name])
      stream.write('mem %x %s\n' % (MEM_BASE, out['mem'].hex()))
      stream.write('faults 0\nend\n')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv))
