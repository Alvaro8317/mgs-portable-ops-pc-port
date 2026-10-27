#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the stage overlay side of generate.py and build_overlays.py (F13).

Synthetic inputs only: a tiny mgp_main and stage PRX (synthetic_prx), a _zar
holding the stage's .rlc and a supported.toml/stages.toml in a temporary
directory, laid out like an installed game.
"""

import hashlib
import importlib.util
import pathlib
import struct
import tempfile
import unittest
import zlib

_PROFILE = pathlib.Path(__file__).resolve().parent.parent


def _load(path):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(path.stem, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


generate = _load(_PROFILE / 'scripts' / 'generate.py')
build_overlays = _load(_PROFILE / 'scripts' / 'build_overlays.py')
psp_prx = _load(_PROFILE / 'tools' / 'psp_prx.py')
rlc = _load(_PROFILE / 'tools' / 'rlc.py')
synthetic_prx = _load(_PROFILE / 'tools' / 'synthetic_prx.py')

_MAIN_BASE = 0x08804000
_R26, _HI16, _LO16 = rlc.R_MIPS_26, rlc.R_MIPS_HI16, rlc.R_MIPS_LO16


def _info(kind, ofs_base, addr_base):
  return synthetic_prx.info(kind, ofs_base, addr_base)


def _main_prx():
  return synthetic_prx.build([0] * 0x40, data=b'\0' * 0x40, bss=0x1000, name='mgp_main')


def _stage_prx():
  # A jal into the stage itself, a jal into mgp_main and a lui/sw pair into
  # mgp_main's data (the shape of rlc_test's stage).
  words = [0x0C000004, 0, 0x0C000020, 0, 0x3C030000, 0xAC620020, 0x03E00008, 0]
  return synthetic_prx.build(words, data=struct.pack('<I', 0x10),
                             relocations=[(0, _info(_R26, 0, 0))], name='mgp_stage')


_RLC = b''.join(struct.pack('<II', offset, i) for offset, i in (
    (0x08, _info(_R26, 0, 0)), (0x10, _info(_HI16, 0, 1)), (0x14, _info(_LO16, 0, 1))))


def _zar(entries):
  # A DAR (docs/formats/rlc.md) behind its decompressed size, zlib-compressed.
  dar = bytearray(struct.pack('<I', len(entries)))
  for name, data in entries:
    dar += name + b'\0'
    while len(dar) % 4:
      dar.append(0)
    dar += struct.pack('<I', len(data))
    while len(dar) % 16:
      dar.append(0)
    dar += data + b'\0'
  return struct.pack('<I', len(dar)) + zlib.compress(bytes(dar))


def _sha256(data):
  return hashlib.sha256(data).hexdigest()


class _Game:
  """A temporary installed game with stages and its configuration files."""

  def __init__(self, root, stages, stage_bases=(), wrong=None):
    self.root = root
    self.game = root / 'game'
    (self.game / 'elf').mkdir(parents=True)
    main = _main_prx()
    (self.game / 'elf' / 'mgp_main.elf').write_bytes(main)
    files = []
    for stage in stages:
      directory = self.game / 'disc' / 'PSP_GAME' / 'USRDIR' / 'stage' / stage
      directory.mkdir(parents=True)
      prx, zar = _stage_prx(), _zar([(b'data.cnf', b'x\n'), (f'{stage}.rlc'.encode(), _RLC)])
      (directory / f'{stage}.prx').write_bytes(prx)
      (directory / '_zar').write_bytes(zar)
      prx_hash = '0' * 64 if wrong == stage else _sha256(prx)
      files.append(f'{{ path = "PSP_GAME/USRDIR/stage/{stage}/_zar", sha256 = "{_sha256(zar)}" }}')
      files.append(f'{{ path = "PSP_GAME/USRDIR/stage/{stage}/{stage}.prx", sha256 = "{prx_hash}" }}')
    file_list = ',\n  '.join(files)
    self.supported = root / 'supported.toml'
    self.supported.write_text(
        '[[version]]\ndisc_id = "TEST00000"\ndisc_version = "1.00"\n'
        f'decrypt = [ {{ path = "EBOOT.BIN", module = "mgp_main", output = "mgp_main.elf", '
        f'sha256 = "{_sha256(main)}" }} ]\n'
        f'file = [\n  {file_list}\n]\n', encoding='utf-8')
    self.stages = root / 'stages.toml'
    self.stages.write_text(
        'default_base = 0x08B8F900\n' +
        ''.join(f'[[stage]]\nname = "{name}"\nbase = 0x{base:08X}\n' for name, base in stage_bases),
        encoding='utf-8')


class LinkStageTest(unittest.TestCase):

  def test_identity_of_the_linked_stage(self):
    with tempfile.TemporaryDirectory() as tmp:
      game = _Game(pathlib.Path(tmp), ['st99'])
      out = pathlib.Path(tmp) / 'out'
      target, inputs, overlay = generate.link_stage('st99', game.game, game.supported,
                                                    game.stages, out)
      elf, _, elf_sha256, base, prefix, disc = target
      stage, main = psp_prx.Prx(_stage_prx()), psp_prx.Prx(_main_prx())
      linked = rlc.link(stage, _RLC, main, _MAIN_BASE)
      image = rlc.relocate(stage, _RLC, main, 0x08B8F900, _MAIN_BASE)
      text = stage.section('.text')
      self.assertEqual(elf.read_bytes(), linked)
      self.assertEqual(elf_sha256, _sha256(linked))
      self.assertEqual((base, prefix, disc), ('0x08B8F900', 'stage_st99', 'TEST00000 1.00'))
      self.assertEqual(overlay, {
          'name': 'st99',
          'prefix': 'stage_st99',
          'base': '0x08B8F900',
          'size': f'0x{len(image):X}',
          'prx_sha256': _sha256(_stage_prx()),
          'text_offset': '0x0',
          'text_size': f'0x{text.size:X}',
          'text_sha256': _sha256(image[text.addr:text.addr + text.size]),
      })
      # The game's .rlc changes the text: the overlay is checked against the
      # linked code, not the code as loaded.
      loaded = stage.image()
      self.assertNotEqual(overlay['text_sha256'], _sha256(loaded[:text.size]))
      self.assertEqual(set(inputs), {
          'game/elf/mgp_main.elf', 'game/disc/PSP_GAME/USRDIR/stage/st99/st99.prx',
          'game/disc/PSP_GAME/USRDIR/stage/st99/_zar'})

  def test_base_from_stages_toml(self):
    with tempfile.TemporaryDirectory() as tmp:
      game = _Game(pathlib.Path(tmp), ['init'], stage_bases=[('init', 0x08AFB800)])
      target, _, overlay = generate.link_stage('init', game.game, game.supported, game.stages,
                                               pathlib.Path(tmp) / 'out')
      self.assertEqual(target[3], '0x08AFB800')
      self.assertEqual(overlay['base'], '0x08AFB800')
      self.assertEqual(generate.stage_base('other', game.stages), 0x08B8F900)

  def test_rejects(self):
    with tempfile.TemporaryDirectory() as tmp:
      game = _Game(pathlib.Path(tmp), ['st99', 'bad'], wrong='bad')
      out = pathlib.Path(tmp) / 'out'
      for stage in ('bad', 'missing', '../st99', ''):
        with self.subTest(stage=stage):
          with self.assertRaises(generate.GenerateError):
            generate.link_stage(stage, game.game, game.supported, game.stages, out)


class BuildOverlaysTest(unittest.TestCase):

  def test_all_stages(self):
    with tempfile.TemporaryDirectory() as tmp:
      game = _Game(pathlib.Path(tmp), ['title', 'init', 'st01'])
      self.assertEqual(build_overlays.all_stages(game.supported), ['init', 'st01', 'title'])

  def test_stages_with_the_same_prx_and_base_share_a_corpus(self):
    with tempfile.TemporaryDirectory() as tmp:
      # Same PRX everywhere; r_main01 is recompiled for another base.
      game = _Game(pathlib.Path(tmp), ['r_main01', 'r_net01', 'st08', 'st0801'],
                   stage_bases=[('r_main01', 0x08AFB800)])
      kept, aliases = build_overlays.distinct_stages(
          ['r_main01', 'r_net01', 'st08', 'st0801'], game.supported, game.stages, generate)
      self.assertEqual(kept, ['r_main01', 'r_net01'])
      self.assertEqual(aliases, {'st08': 'r_net01', 'st0801': 'r_net01'})
      with self.assertRaises(generate.GenerateError):
        build_overlays.distinct_stages(['nope'], game.supported, game.stages, generate)


class ArgumentsTest(unittest.TestCase):

  def test_stage_output_directory(self):
    args = generate.parse_args(['--psp-recomp', 'x', '--stage', 'title'])
    self.assertEqual(pathlib.Path(args.out).parts[-3:], ('generated', 'overlays', 'title'))
    args = generate.parse_args(['--psp-recomp', 'x'])
    self.assertEqual(pathlib.Path(args.out).parts[-2:], ('generated', 'mgp_main'))


if __name__ == '__main__':
  unittest.main()
