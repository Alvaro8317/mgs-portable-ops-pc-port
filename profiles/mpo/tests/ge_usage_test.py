#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for profiles/mpo/tools/ppdmp.py and ge_usage.py.

Every `.ppdmp` here is synthetic, built by `DumpBuilder` following the
layout of PPSSPP v1.20.4 (GPU/Debugger/Record.cpp). No game data is used.
"""

import contextlib
import importlib.util
import io
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load_tool(name):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


ge_usage = _load_tool('ge_usage')
ppdmp = ge_usage.ppdmp

# Record types (GPU/Debugger/RecordFormat.h).
INIT, REGISTERS, VERTICES, INDICES, CLUT, TRANSFERSRC = range(6)
MEMSET, MEMCPYDEST, MEMCPYDATA, DISPLAY, CLUTADDR, EDRAMTRANS = range(6, 12)
TEXTURE0, FRAMEBUF0 = 0x10, 0x18

PIXEL_MARKER = b'PIXELMARKER!'


def zstd_raw(data):
  """A zstd frame made of raw blocks (valid input for any zstd decoder)."""
  out = struct.pack('<IB', 0xFD2FB528, 0xA0) + struct.pack('<I', len(data))
  chunks = [data[i:i + 0x20000] for i in range(0, len(data), 0x20000)] or [b'']
  for index, chunk in enumerate(chunks):
    last = 1 if index == len(chunks) - 1 else 0
    out += (last | (len(chunk) << 3)).to_bytes(3, 'little') + chunk
  return out


def _varint(value):
  out = bytearray()
  while True:
    byte = value & 0x7F
    value >>= 7
    out.append(byte | (0x80 if value else 0))
    if not value:
      return bytes(out)


def snappy_literals(data):
  """A snappy block made of literal elements only."""
  out = bytearray(_varint(len(data)))
  for index in range(0, len(data), 60):
    chunk = data[index:index + 60]
    out.append((len(chunk) - 1) << 2)
    out += chunk
  return bytes(out)


class DumpBuilder:
  """Builds synthetic `.ppdmp` files record by record."""

  def __init__(self):
    self.records = []
    self.pushbuf = bytearray()

  def add(self, kind, payload):
    self.records.append((kind, len(payload), len(self.pushbuf)))
    self.pushbuf += payload
    return self

  def init(self, values=None):
    values = values or {}
    words = [0] * 512
    pos = ge_usage.INIT_REGISTER_OFFSET
    for first, last in ge_usage.INIT_RANGES:
      for cmd in range(first, last + 1):
        words[pos] = (cmd << 24) | values.get(cmd, 0)
        pos += 1
    return self.add(INIT, struct.pack('<512I', *words))

  def regs(self, *ops):
    payload = b''.join(
        struct.pack('<I', (cmd << 24) | (param & 0xFFFFFF))
        for cmd, param in ops)
    return self.add(REGISTERS, payload)

  def build(self, version=6, game_id=b'ULUS10202', compress=None):
    table = b''.join(
        struct.pack('<BII', kind, size, ptr)
        for kind, size, ptr in self.records)
    if compress is None:
      compress = zstd_raw if version >= 5 else snappy_literals
    out = b'PPSSPPGE' + struct.pack('<I', version)
    if version >= 4:
      out += game_id.ljust(9, b'\0')[:9] + b'\0\0\0'
    out += struct.pack('<II', len(self.records), len(self.pushbuf))
    for blob in (table, bytes(self.pushbuf)):
      packed = compress(blob)
      out += struct.pack('<I', len(packed)) + packed
    return out


# Register values for a 480x272 8888 frame at VRAM+0 with depth at +0x88000.
BASE_STATE = {
    0x9C: 0x000000,  # FRAMEBUFPTR
    0x9D: 512,  # FRAMEBUFWIDTH
    0xD2: 3,  # FRAMEBUFPIXFORMAT 8888
    0x9E: 0x088000,  # ZBUFPTR
    0x9F: 512,  # ZBUFWIDTH
    0xD4: 0,  # SCISSOR1
    0xD5: 479 | (271 << 10),  # SCISSOR2
    0x16: 479 | (271 << 10),  # REGION2
    0xC5: 0x00FF00 | 3,  # CLUTFORMAT: 8888, simple index
    0xD7: 0xFFFF,  # MAXZ
}

VTYPE_TEXTURED = 2 | (7 << 2) | (3 << 7)  # tc 16-bit, color 8888, pos float
VTYPE_SKINNED = (3 << 7) | (3 << 9)  # pos float, weights float x1
VTYPE_THROUGH = 2 | (2 << 7) | (1 << 23)  # tc 16-bit, pos 16-bit, through


def prim(kind, count):
  return (0x04, (kind << 16) | count)


def scene_dump(version=6):
  """A frame that touches every section of the report."""
  b = DumpBuilder().init(BASE_STATE)
  b.add(EDRAMTRANS, struct.pack('<I', 0x400))
  # Clear color, stencil and depth.
  b.regs((0x12, VTYPE_THROUGH), (0xD3, 0x701), prim(6, 2), (0xD3, 0))
  # CLUT8 texture from RAM with an 8888 palette from RAM.
  b.regs((0xB0, 0x880000), (0xB1, 0x080000))
  b.add(CLUT, bytes(1024))
  b.regs((0xC4, 32), (0x1E, 1), (0xC3, 5), (0xA0, 0x900000),
         (0xA8, (0x08 << 16) | 64), (0xB8, 6 | (6 << 8)), (0xC2, 1),
         (0xC9, 0x100), (0x12, VTYPE_TEXTURED), (0x21, 1),
         (0xDF, 2 | (3 << 4)), (0x22, 1), (0xDB, 6 | (0xFF << 16)),
         (0x23, 1), (0xDE, 5))
  b.add(TEXTURE0, PIXEL_MARKER * 4)
  b.add(VERTICES, bytes(30 * 20))
  b.regs(prim(3, 30))
  # Render into a 565 target at VRAM+0x154000 with a logic op and stencil.
  b.regs((0x9C, 0x154000), (0x9D, 256), (0xD2, 0), (0x1E, 0), (0x21, 0),
         (0x22, 0), (0x28, 1), (0xE6, 6), (0x24, 1),
         (0xDC, 2 | (1 << 8) | (0xFF << 16)), (0xDD, 2 << 16),
         (0x12, 3 << 7))
  b.add(VERTICES, bytes(4 * 12))
  b.regs(prim(4, 4))
  # Back to the main target, sample the 565 target (render-to-texture).
  b.regs((0x9C, 0x000000), (0x9D, 512), (0xD2, 3), (0x28, 0), (0x24, 0),
         (0x1E, 1), (0xC3, 0), (0xA0, 0x154000), (0xA8, (0x04 << 16) | 256),
         (0xB8, 8 | (8 << 8)), (0xC2, 0), (0xC9, 0x10003),
         (0x12, VTYPE_THROUGH))
  b.add(FRAMEBUF0, struct.pack('<IiII', 0x04154000, 256, 5, 0) + PIXEL_MARKER)
  b.add(VERTICES, bytes(2 * 8))
  b.regs(prim(6, 2))
  # Sample the current target itself (self-texturing).
  b.regs((0xA0, 0x000000), (0xA8, (0x04 << 16) | 512), (0xC3, 3))
  b.add(FRAMEBUF0, struct.pack('<IiII', 0x04000000, 512, 5, 0) + PIXEL_MARKER)
  b.regs(prim(6, 2))
  # Skinned bezier and spline patches, then lines.
  b.regs((0x1E, 0), (0x12, VTYPE_SKINNED), (0x36, 8 | (8 << 8)), (0x37, 0),
         (0x1C, 1), (0x05, 4 | (4 << 8)), (0x06, 5 | (4 << 8) | (3 << 16)))
  b.regs((0x12, 3 << 7), prim(1, 8))
  # Block transfer VRAM -> VRAM (32 bpp).
  b.regs((0xB2, 0x000000), (0xB3, 0x040000 | 512), (0xB4, 0x154000),
         (0xB5, 0x040000 | 256), (0xEB, 0), (0xEC, 0),
         (0xEE, 479 | (271 << 10)))
  b.add(TRANSFERSRC, bytes(64))
  b.regs((0xEA, 1))
  # CPU writes into VRAM and the displayed framebuffer.
  b.add(MEMSET, struct.pack('<IiI', 0x04088000, 0, 0x44000))
  b.add(MEMCPYDEST, struct.pack('<I', 0x04000000))
  b.add(MEMCPYDATA, PIXEL_MARKER)
  b.add(DISPLAY, struct.pack('<IiI', 0x04000000, 512, 3))
  return b.build(version=version)


class SnappyTest(unittest.TestCase):

  def test_literals_and_copies(self):
    # "abcd" literal, copy-1 (len 4, off 4), copy-2 (len 3, off 2),
    # copy-4 (len 2, off 1) and a 1-byte-length literal.
    data = (_varint(4 + 4 + 3 + 2 + 61) + bytes([3 << 2]) + b'abcd' +
            bytes([0x01 | (0 << 2), 4]) + bytes([0x02 | (2 << 2)]) +
            struct.pack('<H', 2) + bytes([0x03 | (1 << 2)]) +
            struct.pack('<I', 1) + bytes([60 << 2, 60]) + b'z' * 61)
    self.assertEqual(
        ppdmp.snappy_decompress(data, 74),
        b'abcdabcd' + b'cdc' + b'cc' + b'z' * 61)

  def test_malformed_streams_are_rejected(self):
    literal = bytes([3 << 2]) + b'abcd'
    cases = {
        'length mismatch': (_varint(5) + literal, 4),
        'zero offset': (_varint(8) + literal + b'\x01\x00', 8),
        'offset too far': (_varint(8) + literal + b'\x01\x09', 8),
        'copy past the end': (_varint(6) + literal + b'\x01\x04', 6),
        'truncated literal': (_varint(4) + bytes([3 << 2]) + b'ab', 4),
        'truncated stream': (_varint(8) + literal, 8),
        'bad varint': (b'\xff\xff\xff\xff\xff\xff', 4),
    }
    for name, (data, expected) in cases.items():
      with self.subTest(name=name):
        with self.assertRaises(ppdmp.DumpError):
          ppdmp.snappy_decompress(data, expected)


class ContainerTest(unittest.TestCase):

  def test_versions_and_compression(self):
    for version in (2, 3, 4, 5, 6):
      with self.subTest(version=version):
        raw = DumpBuilder().init().regs(prim(3, 3)).build(version=version)
        dump = ppdmp.parse(raw, force_pure=True)
        self.assertEqual(dump.version, version)
        self.assertEqual(dump.compression,
                         'snappy' if version < 5 else 'zstd')
        self.assertEqual(dump.game_id, 'ULUS10202' if version >= 4 else '')
        self.assertEqual([c.name for c in dump.commands], ['INIT', 'REGISTERS'])

  def test_every_truncation_is_rejected(self):
    raw = DumpBuilder().init().regs(prim(3, 3)).build()
    for length in range(len(raw)):
      with self.subTest(length=length):
        with self.assertRaises(ppdmp.DumpError):
          ppdmp.parse(raw[:length], force_pure=True)

  def test_invalid_files_are_rejected(self):
    good = DumpBuilder().init().build()
    bad_version = bytearray(good)
    bad_version[8] = 7
    old_version = bytearray(good)
    old_version[8] = 1
    outside = DumpBuilder().init()
    outside.records.append((REGISTERS, 8, len(outside.pushbuf) - 4))
    unknown = DumpBuilder().init()
    unknown.records.append((0x0C, 0, 0))
    # Header says 1 command but the table decompresses to 2.
    wrong_count = bytearray(DumpBuilder().init().regs(prim(3, 3)).build())
    struct.pack_into('<I', wrong_count, 24, 1)
    cases = {
        'bad magic': b'PPSSPPXX' + good[8:],
        'version 7': bytes(bad_version),
        'version 1': bytes(old_version),
        'trailing bytes': good + b'\0',
        'pointer outside pushbuf': outside.build(),
        'unknown record type': unknown.build(),
        'command count mismatch': bytes(wrong_count),
    }
    for name, raw in cases.items():
      with self.subTest(name=name):
        with self.assertRaises(ppdmp.DumpError):
          ppdmp.parse(raw, force_pure=True)

  def test_pushbuf_cap(self):
    raw = DumpBuilder().init().build()
    with self.assertRaisesRegex(ppdmp.DumpError, 'exceeds the limit'):
      ppdmp.parse(raw, max_pushbuf=1024)


class WalkerTest(unittest.TestCase):

  def setUp(self):
    dump = ppdmp.parse(scene_dump(), force_pure=True)
    self.report = ge_usage.analyze(dump, 'scene.ppdmp')
    self.s = self.report.sections

  def test_summary(self):
    (summary,) = self.report.files
    self.assertEqual(summary['game_id'], 'ULUS10202')
    self.assertEqual(summary['draws'], 8)
    self.assertEqual(summary['vertices'], 2 + 30 + 4 + 2 + 2 + 16 + 20 + 8)
    self.assertEqual(summary['render_targets'], 2)

  def test_primitives_and_vertex_types(self):
    self.assertEqual(self.s['prims']['RECTANGLES through'], 3)
    self.assertEqual(self.s['prims']['TRIANGLES transform'], 1)
    self.assertEqual(self.s['prims']['TRIANGLE_STRIP transform'], 1)
    self.assertEqual(self.s['prims']['LINES transform'], 1)
    self.assertEqual(self.s['prim_vertices']['TRIANGLES transform'], 30)
    self.assertEqual(
        self.s['vtypes']['0x00019E tc=16bit col=8888 pos=float transform '
                         'size=20'], 1)
    self.assertEqual(
        self.s['vtypes']['0x000780 pos=float weights=floatx1 transform '
                         'size=16'], 2)

  def test_patches(self):
    self.assertEqual(self.s['patches']['BEZIER 4x4 div=8x8 prim=TRIANGLES'], 1)
    self.assertEqual(
        self.s['patches']['SPLINE 5x4 div=8x8 prim=TRIANGLES type=3x0'], 1)
    for feature in ('bezier', 'spline', 'skinning', 'lines', 'depth clamp'):
      with self.subTest(feature=feature):
        self.assertGreater(self.s['features'][feature], 0)

  def test_textures(self):
    self.assertEqual(self.s['tex_format']['CLUT8 (palette 8888)'], 1)
    self.assertEqual(self.s['tex_format']['5650'], 1)
    self.assertEqual(self.s['tex_format']['8888'], 1)
    self.assertEqual(self.s['tex_clut']['simple'], 1)
    self.assertEqual(self.s['tex_mode']['swizzled levels=1'], 1)
    self.assertEqual(self.s['tex_func']['MODULATE rgba'], 1)
    self.assertEqual(self.s['tex_func']['REPLACE rgb x2'], 2)
    self.assertEqual(self.s['tex_source']['RAM'], 1)
    self.assertEqual(self.s['clut_loads']['1024 bytes from RAM'], 1)
    self.assertEqual(sum(self.s['unique_textures'].values()), 3)

  def test_render_to_texture_and_vram_reads(self):
    features = self.s['features']
    self.assertEqual(features['render-to-texture'], 1)
    self.assertEqual(features['self-texturing (texture = current target)'], 1)
    self.assertEqual(features['texture from VRAM'], 2)
    self.assertEqual(self.s['tex_source']['VRAM: render target 0x154000 565'],
                     1)
    self.assertIn(
        'texture 5650 256x256 @ VRAM+0x154000 mirror=0: render target '
        '0x154000 565 recorder-flags=TD', self.s['vram_reads'])
    self.assertEqual(self.s['render_targets']['0x154000 565 stride=256'], 1)
    self.assertEqual(features['565 framebuffer'], 1)

  def test_pixel_pipeline(self):
    self.assertEqual(self.s['blend']['ADD(SRCALPHA, INVSRCALPHA)'], 1)
    self.assertEqual(self.s['logic_op']['XOR'], 1)
    self.assertEqual(
        self.s['stencil']['EQUAL ref=0x01 mask=0xFF sfail=KEEP zfail=KEEP '
                          'zpass=REPLACE fb=565'], 1)
    self.assertEqual(self.s['alpha_test']['GREATER ref=0x00 mask=0xFF'], 1)
    self.assertEqual(self.s['features']['stencil write'], 1)
    self.assertEqual(self.s['clears']['color+alpha/stencil+depth 8888'], 1)
    self.assertEqual(self.s['depth_buffers']['0x088000 stride=512'], 1 + 7)

  def test_transfers_cpu_writes_and_display(self):
    self.assertEqual(
        self.s['transfers']['VRAM (render target 0x000000 8888) -> VRAM '
                            '(render target 0x154000 565) 480x272 bpp=4'], 1)
    self.assertIn(
        'memset 278528 bytes -> VRAM+0x088000 (depth buffer 0x088000) '
        'value=0x00', self.s['cpu_vram_writes'])
    self.assertEqual(self.s['features']['CPU writes into a render target'], 1)
    self.assertEqual(self.s['display']['0x04000000 8888 stride=512'], 1)
    self.assertEqual(self.s['edram_translation']['0x400'], 1)

  def test_ge_command_histogram(self):
    self.assertEqual(self.s['ge_commands']['PRIM'], 6)
    self.assertEqual(self.s['ge_commands']['TEXADDR0'], 3)
    self.assertEqual(self.s['dump_records']['FRAMEBUF0'], 2)

  def test_snappy_dump_gives_the_same_report(self):
    report = ge_usage.analyze(ppdmp.parse(scene_dump(version=4)), 'x')
    for key, counter in self.s.items():
      with self.subTest(section=key):
        self.assertEqual(report.sections[key], counter)


class WalkerValidationTest(unittest.TestCase):

  def _analyze(self, builder):
    return ge_usage.analyze(ppdmp.parse(builder.build(), force_pure=True), 'x')

  def test_malformed_records_are_rejected(self):
    bad_init = DumpBuilder()
    bad_init.add(INIT, bytes(512 * 4))  # Register values lack their command.
    cases = {
        'short init': DumpBuilder().add(INIT, bytes(16)),
        'init without commands': bad_init,
        'unaligned registers': DumpBuilder().init().add(REGISTERS, b'\0\0\0'),
        'short display': DumpBuilder().init().add(DISPLAY, bytes(8)),
        'short framebuf': DumpBuilder().init().add(FRAMEBUF0, bytes(8)),
        'memcpy without dest': DumpBuilder().init().add(MEMCPYDATA, b'ab'),
        'short clutaddr': DumpBuilder().init().add(CLUTADDR, bytes(4)),
        'short memset': DumpBuilder().init().add(MEMSET, bytes(4)),
    }
    for name, builder in cases.items():
      with self.subTest(name=name):
        with self.assertRaises(ppdmp.DumpError):
          self._analyze(builder)

  def test_texture_from_previously_displayed_frame(self):
    builder = DumpBuilder().init(BASE_STATE)
    builder.add(DISPLAY, struct.pack('<IiI', 0x04100000, 512, 3))
    builder.regs((0x1E, 1), (0xC3, 3), (0xA0, 0x101000),
                 (0xA8, (0x04 << 16) | 512), (0xB8, 9 | (8 << 8)),
                 prim(6, 2))
    report = self._analyze(builder)
    self.assertEqual(
        report.sections['features']['texture from displayed framebuffer'], 1)
    self.assertEqual(
        report.sections['tex_source']['VRAM: displayed framebuffer'], 1)

  def test_keep_previous_primitive(self):
    report = self._analyze(DumpBuilder().init().regs(prim(1, 2), prim(7, 4)))
    self.assertEqual(report.sections['prims']['LINES transform'], 2)

  def test_vertex_sizes_match_ppsspp_rules(self):
    cases = {
        3 << 7: 12,  # float position
        (1 << 7) | 1: 5,  # 8-bit tc + 8-bit position
        (2 << 7) | (4 << 2): 8,  # 565 color + 16-bit position
        (3 << 7) | (1 << 9) | (3 << 14): 16,  # 4 x 8-bit weights
        (3 << 7) | (1 << 18): 24,  # two morph targets
        (1 << 7) | (1 << 5): 6,  # 8-bit normal + 8-bit position
    }
    for vtype, size in cases.items():
      with self.subTest(vtype=hex(vtype)):
        self.assertEqual(ge_usage.vertex_size(vtype), size)


class CommandLineTest(unittest.TestCase):

  def setUp(self):
    self.tmp = tempfile.TemporaryDirectory()
    self.dir = pathlib.Path(self.tmp.name)
    (self.dir / 'a.ppdmp').write_bytes(scene_dump())
    (self.dir / 'b.ppdmp').write_bytes(scene_dump(version=3))

  def tearDown(self):
    self.tmp.cleanup()

  def _run(self, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
      code = ge_usage.main(list(args))
    return code, out.getvalue(), err.getvalue()

  def test_formats_contain_no_pixel_data(self):
    for fmt in ('text', 'markdown', 'json'):
      with self.subTest(fmt=fmt):
        code, out, _ = self._run('--format', fmt, '--per-file',
                                 '--pure-python', str(self.dir))
        self.assertEqual(code, 0)
        self.assertIn('render-to-texture', out)
        self.assertNotIn(PIXEL_MARKER.decode(), out)
        self.assertNotIn(PIXEL_MARKER.hex(), out.lower())

  def test_json_totals_merge_files(self):
    code, out, _ = self._run('--format', 'json', str(self.dir))
    self.assertEqual(code, 0)
    data = json.loads(out)
    self.assertEqual(len(data['total']['files']), 2)
    self.assertEqual(data['total']['sections']['prims']['LINES transform'], 2)

  def test_bad_file_is_reported_and_others_still_count(self):
    (self.dir / 'c.ppdmp').write_bytes(b'PPSSPPGE\x06\0\0\0')
    code, out, err = self._run(str(self.dir))
    self.assertEqual(code, 1)
    self.assertIn('c.ppdmp', err)
    self.assertIn('All dumps (2)', out)

  def test_no_input(self):
    code, _, err = self._run(str(self.dir / 'empty_dir_missing'))
    self.assertEqual(code, 1)
    self.assertIn('error', err)
    empty = self.dir / 'empty'
    empty.mkdir()
    code, _, _ = self._run(str(empty))
    self.assertEqual(code, 2)

  def test_runs_isolated(self):
    result = subprocess.run(
        [sys.executable, '-I', str(_TOOLS / 'ge_usage.py'), str(self.dir)],
        capture_output=True, text=True, check=False)
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assertIn('All dumps (2)', result.stdout)


if __name__ == '__main__':
  unittest.main()
