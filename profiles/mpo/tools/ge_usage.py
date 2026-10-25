#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Histogram of the GE features used by PPSSPP frame dumps (`.ppdmp`).

Walks the recorded command stream of each dump, keeps the GE register state
the same way the GE does (last value written per command) and, at every draw
(PRIM, BEZIER, SPLINE), records the state that matters for a renderer:
primitives and vertex types, texture formats and CLUT, blending, logic ops,
stencil, alpha/color test, depth, splines/beziers, render targets, and
texture/CLUT reads from VRAM (render-to-texture). It also lists block
transfers, CPU writes to VRAM and the displayed framebuffer.

Only numbers and names come out: no vertex, texture or pixel data. The
input is untrusted; see `ppdmp.py` for the container checks.

GE command numbers, field layouts, the INIT record layout and the vertex size
rules follow PPSSPP v1.20.4 (GPL-2.0-or-later): GPU/ge_constants.h,
GPU/GPUState.{h,cpp}, GPU/Common/VertexDecoderCommon.cpp and
GPU/Debugger/Record.cpp. No code was copied; the tables were rewritten here.

Usage:
  python -I profiles/mpo/tools/ge_usage.py dumps/*.ppdmp
  python -I profiles/mpo/tools/ge_usage.py --format markdown --per-file dumps/
  python -I profiles/mpo/tools/ge_usage.py --format json dumps/ > ge.json
"""

import argparse
import collections
import importlib.util
import json
import pathlib
import struct
import sys


def _load_sibling(name):
  # Loaded by path so the tool also works under `python -I`.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(f'mpo_tools_{name}', path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


ppdmp = _load_sibling('ppdmp')

# GE command numbers (PPSSPP v1.20.4 GPU/ge_constants.h).
CMD_PRIM = 0x04
CMD_BEZIER = 0x05
CMD_SPLINE = 0x06
CMD_VERTEXTYPE = 0x12
CMD_REGION1 = 0x15
CMD_REGION2 = 0x16
CMD_LIGHTINGENABLE = 0x17
CMD_LIGHTENABLE0 = 0x18
CMD_DEPTHCLAMPENABLE = 0x1C
CMD_CULLFACEENABLE = 0x1D
CMD_TEXTUREMAPENABLE = 0x1E
CMD_FOGENABLE = 0x1F
CMD_DITHERENABLE = 0x20
CMD_ALPHABLENDENABLE = 0x21
CMD_ALPHATESTENABLE = 0x22
CMD_ZTESTENABLE = 0x23
CMD_STENCILTESTENABLE = 0x24
CMD_ANTIALIASENABLE = 0x25
CMD_PATCHCULLENABLE = 0x26
CMD_COLORTESTENABLE = 0x27
CMD_LOGICOPENABLE = 0x28
CMD_PATCHDIVISION = 0x36
CMD_PATCHPRIMITIVE = 0x37
CMD_PATCHFACING = 0x38
CMD_SHADEMODE = 0x50
CMD_LIGHTTYPE0 = 0x5F
CMD_CULL = 0x9B
CMD_FRAMEBUFPTR = 0x9C
CMD_FRAMEBUFWIDTH = 0x9D
CMD_ZBUFPTR = 0x9E
CMD_ZBUFWIDTH = 0x9F
CMD_TEXADDR0 = 0xA0
CMD_TEXBUFWIDTH0 = 0xA8
CMD_TRANSFERSRC = 0xB2
CMD_TRANSFERSRCW = 0xB3
CMD_TRANSFERDST = 0xB4
CMD_TRANSFERDSTW = 0xB5
CMD_TEXSIZE0 = 0xB8
CMD_TEXMAPMODE = 0xC0
CMD_TEXMODE = 0xC2
CMD_TEXFORMAT = 0xC3
CMD_LOADCLUT = 0xC4
CMD_CLUTFORMAT = 0xC5
CMD_TEXFILTER = 0xC6
CMD_TEXWRAP = 0xC7
CMD_TEXLEVEL = 0xC8
CMD_TEXFUNC = 0xC9
CMD_FRAMEBUFPIXFORMAT = 0xD2
CMD_CLEARMODE = 0xD3
CMD_SCISSOR1 = 0xD4
CMD_SCISSOR2 = 0xD5
CMD_MINZ = 0xD6
CMD_MAXZ = 0xD7
CMD_COLORTEST = 0xD8
CMD_COLORREF = 0xD9
CMD_COLORTESTMASK = 0xDA
CMD_ALPHATEST = 0xDB
CMD_STENCILTEST = 0xDC
CMD_STENCILOP = 0xDD
CMD_ZTEST = 0xDE
CMD_BLENDMODE = 0xDF
CMD_BLENDFIXEDA = 0xE0
CMD_BLENDFIXEDB = 0xE1
CMD_LOGICOP = 0xE6
CMD_ZWRITEDISABLE = 0xE7
CMD_MASKRGB = 0xE8
CMD_MASKALPHA = 0xE9
CMD_TRANSFERSTART = 0xEA
CMD_TRANSFERSRCPOS = 0xEB
CMD_TRANSFERDSTPOS = 0xEC
CMD_TRANSFERSIZE = 0xEE

_NAMED_COMMANDS = {
    0x00: 'NOP', 0x01: 'VADDR', 0x02: 'IADDR', 0x04: 'PRIM', 0x05: 'BEZIER',
    0x06: 'SPLINE', 0x07: 'BOUNDINGBOX', 0x08: 'JUMP', 0x09: 'BJUMP',
    0x0A: 'CALL', 0x0B: 'RET', 0x0C: 'END', 0x0E: 'SIGNAL', 0x0F: 'FINISH',
    0x10: 'BASE', 0x12: 'VERTEXTYPE', 0x13: 'OFFSETADDR', 0x14: 'ORIGIN',
    0x15: 'REGION1', 0x16: 'REGION2', 0x17: 'LIGHTINGENABLE',
    0x1C: 'DEPTHCLAMPENABLE', 0x1D: 'CULLFACEENABLE',
    0x1E: 'TEXTUREMAPENABLE', 0x1F: 'FOGENABLE', 0x20: 'DITHERENABLE',
    0x21: 'ALPHABLENDENABLE', 0x22: 'ALPHATESTENABLE', 0x23: 'ZTESTENABLE',
    0x24: 'STENCILTESTENABLE', 0x25: 'ANTIALIASENABLE',
    0x26: 'PATCHCULLENABLE', 0x27: 'COLORTESTENABLE', 0x28: 'LOGICOPENABLE',
    0x2A: 'BONEMATRIXNUMBER', 0x2B: 'BONEMATRIXDATA', 0x36: 'PATCHDIVISION',
    0x37: 'PATCHPRIMITIVE', 0x38: 'PATCHFACING', 0x3A: 'WORLDMATRIXNUMBER',
    0x3B: 'WORLDMATRIXDATA', 0x3C: 'VIEWMATRIXNUMBER',
    0x3D: 'VIEWMATRIXDATA', 0x3E: 'PROJMATRIXNUMBER',
    0x3F: 'PROJMATRIXDATA', 0x40: 'TGENMATRIXNUMBER',
    0x41: 'TGENMATRIXDATA', 0x42: 'VIEWPORTXSCALE', 0x43: 'VIEWPORTYSCALE',
    0x44: 'VIEWPORTZSCALE', 0x45: 'VIEWPORTXCENTER',
    0x46: 'VIEWPORTYCENTER', 0x47: 'VIEWPORTZCENTER', 0x48: 'TEXSCALEU',
    0x49: 'TEXSCALEV', 0x4A: 'TEXOFFSETU', 0x4B: 'TEXOFFSETV',
    0x4C: 'OFFSETX', 0x4D: 'OFFSETY', 0x50: 'SHADEMODE',
    0x51: 'REVERSENORMAL', 0x53: 'MATERIALUPDATE',
    0x54: 'MATERIALEMISSIVE', 0x55: 'MATERIALAMBIENT',
    0x56: 'MATERIALDIFFUSE', 0x57: 'MATERIALSPECULAR',
    0x58: 'MATERIALALPHA', 0x5B: 'MATERIALSPECULARCOEF',
    0x5C: 'AMBIENTCOLOR', 0x5D: 'AMBIENTALPHA', 0x5E: 'LIGHTMODE',
    0x9B: 'CULL', 0x9C: 'FRAMEBUFPTR', 0x9D: 'FRAMEBUFWIDTH',
    0x9E: 'ZBUFPTR', 0x9F: 'ZBUFWIDTH', 0xB0: 'CLUTADDR',
    0xB1: 'CLUTADDRUPPER', 0xB2: 'TRANSFERSRC', 0xB3: 'TRANSFERSRCW',
    0xB4: 'TRANSFERDST', 0xB5: 'TRANSFERDSTW', 0xC0: 'TEXMAPMODE',
    0xC1: 'TEXSHADELS', 0xC2: 'TEXMODE', 0xC3: 'TEXFORMAT',
    0xC4: 'LOADCLUT', 0xC5: 'CLUTFORMAT', 0xC6: 'TEXFILTER',
    0xC7: 'TEXWRAP', 0xC8: 'TEXLEVEL', 0xC9: 'TEXFUNC',
    0xCA: 'TEXENVCOLOR', 0xCB: 'TEXFLUSH', 0xCC: 'TEXSYNC', 0xCD: 'FOG1',
    0xCE: 'FOG2', 0xCF: 'FOGCOLOR', 0xD0: 'TEXLODSLOPE',
    0xD2: 'FRAMEBUFPIXFORMAT', 0xD3: 'CLEARMODE', 0xD4: 'SCISSOR1',
    0xD5: 'SCISSOR2', 0xD6: 'MINZ', 0xD7: 'MAXZ', 0xD8: 'COLORTEST',
    0xD9: 'COLORREF', 0xDA: 'COLORTESTMASK', 0xDB: 'ALPHATEST',
    0xDC: 'STENCILTEST', 0xDD: 'STENCILOP', 0xDE: 'ZTEST',
    0xDF: 'BLENDMODE', 0xE0: 'BLENDFIXEDA', 0xE1: 'BLENDFIXEDB',
    0xE6: 'LOGICOP', 0xE7: 'ZWRITEDISABLE', 0xE8: 'MASKRGB',
    0xE9: 'MASKALPHA', 0xEA: 'TRANSFERSTART', 0xEB: 'TRANSFERSRCPOS',
    0xEC: 'TRANSFERDSTPOS', 0xEE: 'TRANSFERSIZE', 0xF0: 'VSCX',
    0xF1: 'VSCY', 0xF2: 'VSCZ', 0xF3: 'VTCS', 0xF4: 'VTCT', 0xF5: 'VTCQ',
    0xF6: 'VCV', 0xF7: 'VAP', 0xF8: 'VFC', 0xF9: 'VSCV', 0xFF: 'NOP_FF',
}
_RANGE_NAMES = (
    (0x18, 4, 'LIGHTENABLE'), (0x2C, 8, 'MORPHWEIGHT'), (0x5F, 4, 'LIGHTTYPE'),
    (0x63, 12, 'LIGHTPOS'), (0x6F, 12, 'LIGHTDIR'), (0x7B, 12, 'LIGHTATT'),
    (0x87, 4, 'LIGHTSPOTEXP'), (0x8B, 4, 'LIGHTSPOTCUTOFF'),
    (0x8F, 12, 'LIGHTCOLOR'), (0xA0, 8, 'TEXADDR'), (0xA8, 8, 'TEXBUFWIDTH'),
    (0xB8, 8, 'TEXSIZE'), (0xE2, 4, 'DITH'),
)


def command_name(cmd):
  """Returns the PPSSPP name of GE command number `cmd`."""
  if cmd in _NAMED_COMMANDS:
    return _NAMED_COMMANDS[cmd]
  for first, count, name in _RANGE_NAMES:
    if first <= cmd < first + count:
      return f'{name}{cmd - first}'
  return f'UNKNOWN_{cmd:02X}'


PRIM_TYPES = ('POINTS', 'LINES', 'LINE_STRIP', 'TRIANGLES', 'TRIANGLE_STRIP',
              'TRIANGLE_FAN', 'RECTANGLES', 'KEEP_PREVIOUS')
TEXTURE_FORMATS = ('5650', '5551', '4444', '8888', 'CLUT4', 'CLUT8', 'CLUT16',
                   'CLUT32', 'DXT1', 'DXT3', 'DXT5')
CLUT_FORMATS = ('5650', '5551', '4444', '8888')
BUFFER_FORMATS = ('565', '5551', '4444', '8888')
COMPARISONS = ('NEVER', 'ALWAYS', 'EQUAL', 'NOTEQUAL', 'LESS', 'LEQUAL',
               'GREATER', 'GEQUAL')
BLEND_EQUATIONS = ('ADD', 'SUBTRACT', 'REVERSE_SUBTRACT', 'MIN', 'MAX',
                   'ABSDIFF')
BLEND_SRC = ('DSTCOLOR', 'INVDSTCOLOR', 'SRCALPHA', 'INVSRCALPHA', 'DSTALPHA',
             'INVDSTALPHA', 'DOUBLESRCALPHA', 'DOUBLEINVSRCALPHA',
             'DOUBLEDSTALPHA', 'DOUBLEINVDSTALPHA', 'FIXA')
BLEND_DST = ('SRCCOLOR', 'INVSRCCOLOR', 'SRCALPHA', 'INVSRCALPHA', 'DSTALPHA',
             'INVDSTALPHA', 'DOUBLESRCALPHA', 'DOUBLEINVSRCALPHA',
             'DOUBLEDSTALPHA', 'DOUBLEINVDSTALPHA', 'FIXB')
LOGIC_OPS = ('CLEAR', 'AND', 'AND_REVERSE', 'COPY', 'AND_INVERTED', 'NOOP',
             'XOR', 'OR', 'NOR', 'EQUIV', 'INVERTED', 'OR_REVERSE',
             'COPY_INVERTED', 'OR_INVERTED', 'NAND', 'SET')
STENCIL_OPS = ('KEEP', 'ZERO', 'REPLACE', 'INVERT', 'INCR', 'DECR', 'OP6',
               'OP7')
TEX_FUNCS = ('MODULATE', 'DECAL', 'BLEND', 'REPLACE', 'ADD', 'UNKNOWN5',
             'UNKNOWN6', 'UNKNOWN7')
TEX_FILTERS = {0: 'NEAREST', 1: 'LINEAR', 4: 'NEAREST_MIPMAP_NEAREST',
               5: 'LINEAR_MIPMAP_NEAREST', 6: 'NEAREST_MIPMAP_LINEAR',
               7: 'LINEAR_MIPMAP_LINEAR'}
TEX_LEVEL_MODES = ('AUTO', 'CONST', 'SLOPE', 'UNKNOWN3')
UV_GEN_MODES = ('COORDS', 'MATRIX', 'ENVMAP', 'UNKNOWN3')
UV_PROJ_MODES = ('POSITION', 'UV', 'NORMALIZED_NORMAL', 'NORMAL')
PATCH_PRIMS = ('TRIANGLES', 'LINES', 'POINTS', 'POINTS(3)')
LIGHT_TYPES = ('DIRECTIONAL', 'POINT', 'SPOT', 'SPOT(3)')
LIGHT_COMPUTATIONS = ('DIFFUSE', 'DIFFUSE+SPECULAR', 'POWERED_DIFFUSE',
                      'UNKNOWN3')
_VTYPE_SIZES = ('none', '8bit', '16bit', 'float')
_VTYPE_COLORS = ('none', 'invalid1', 'invalid2', 'invalid3', '565', '5551',
                 '4444', '8888')

# Sizes and alignments from PPSSPP GPU/Common/VertexDecoderCommon.cpp.
_TC_SIZE = (0, 2, 4, 8)
_COL_SIZE = (0, 0, 0, 0, 2, 2, 2, 4)
_NRM_SIZE = (0, 3, 6, 12)
_POS_SIZE = (3, 3, 6, 12)
_POS_ALIGN = (1, 1, 2, 4)
_WT_SIZE = (0, 1, 2, 4)
_ALIGN_124 = (0, 1, 2, 4)

# INIT record layout (PPSSPP GPU/GPUState.cpp, GPUgstate::Save): 512 words,
# register values start at word 17 and follow these inclusive ranges.
INIT_SIZE = 512 * 4
INIT_REGISTER_OFFSET = 17
INIT_RANGES = ((0x00, 0x02), (0x10, 0x10), (0x12, 0x28), (0x2C, 0x33),
               (0x36, 0x38), (0x42, 0x4D), (0x50, 0x51), (0x53, 0x58),
               (0x5B, 0xB5), (0xB8, 0xC3), (0xC5, 0xD0), (0xD2, 0xE9),
               (0xEB, 0xEC), (0xEE, 0xEE), (0xF0, 0xF6), (0xF8, 0xF9))

VRAM_SIZE = 0x200000
DISPLAY_HEIGHT = 272


def is_vram(address):
  """Mirrors PPSSPP Memory::IsVRAMAddress."""
  return (address & 0x3F800000) == 0x04000000


def _align(value, alignment):
  if alignment <= 1:
    return value
  return (value + alignment - 1) & ~(alignment - 1)


def vertex_size(vtype):
  """Returns the vertex stride in bytes for `vtype` (all morph targets)."""
  tc, col, nrm = vtype & 3, (vtype >> 2) & 7, (vtype >> 5) & 3
  pos, weight = (vtype >> 7) & 3, (vtype >> 9) & 3
  weights = ((vtype >> 14) & 7) + 1
  morphs = ((vtype >> 18) & 7) + 1
  size = 0
  biggest = 0
  if weight:
    size += _WT_SIZE[weight] * weights
    biggest = max(biggest, _ALIGN_124[weight])
  if tc:
    size = _align(size, _ALIGN_124[tc]) + _TC_SIZE[tc]
    biggest = max(biggest, _ALIGN_124[tc])
  if col:
    col_align = _COL_SIZE[col]
    size = _align(size, col_align) + _COL_SIZE[col]
    biggest = max(biggest, col_align)
  if nrm:
    size = _align(size, _ALIGN_124[nrm]) + _NRM_SIZE[nrm]
    biggest = max(biggest, _ALIGN_124[nrm])
  size = _align(size, _POS_ALIGN[pos]) + _POS_SIZE[pos]
  biggest = max(biggest, _POS_ALIGN[pos])
  return _align(size, biggest) * morphs


def describe_vtype(vtype):
  """Returns a readable description of a VERTEXTYPE value."""
  parts = []
  tc, col, nrm = vtype & 3, (vtype >> 2) & 7, (vtype >> 5) & 3
  pos, weight, index = (vtype >> 7) & 3, (vtype >> 9) & 3, (vtype >> 11) & 3
  if tc:
    parts.append(f'tc={_VTYPE_SIZES[tc]}')
  if col:
    parts.append(f'col={_VTYPE_COLORS[col]}')
  if nrm:
    parts.append(f'nrm={_VTYPE_SIZES[nrm]}')
  parts.append(f'pos={_VTYPE_SIZES[pos] if pos else "invalid0"}')
  if weight:
    parts.append(f'weights={_VTYPE_SIZES[weight]}x{((vtype >> 14) & 7) + 1}')
  morphs = ((vtype >> 18) & 7) + 1
  if morphs > 1:
    parts.append(f'morph={morphs}')
  if index:
    parts.append(f'idx={("", "8bit", "16bit", "32bit")[index]}')
  parts.append('through' if vtype & (1 << 23) else 'transform')
  parts.append(f'size={vertex_size(vtype)}')
  return f'0x{vtype & 0xFFFFFF:06X} ' + ' '.join(parts)


def _name(table, index):
  return table[index] if index < len(table) else f'INVALID{index}'


# Sections of the report: (key, title, unit). Every section is a Counter.
SECTIONS = (
    ('features', 'Features (draws that use them)', 'draws'),
    ('dump_records', 'Dump records by type', 'records'),
    ('ge_commands', 'GE commands in the recorded stream', 'commands'),
    ('prims', 'Draws by primitive type and mode', 'draws'),
    ('prim_vertices', 'Vertices by primitive type and mode', 'vertices'),
    ('clears', 'Clear-mode draws (channels written)', 'draws'),
    ('vtypes', 'Vertex types', 'draws'),
    ('patches', 'Beziers and splines', 'draws'),
    ('tex_format', 'Texture formats (textured draws)', 'draws'),
    ('tex_clut', 'CLUT index mode (CLUT textures)', 'draws'),
    ('tex_size', 'Texture sizes (level 0)', 'draws'),
    ('tex_mode', 'Texture swizzle, mip levels and LOD mode', 'draws'),
    ('tex_filter', 'Texture filtering', 'draws'),
    ('tex_wrap', 'Texture wrap', 'draws'),
    ('tex_func', 'Texture function', 'draws'),
    ('tex_uvgen', 'UV generation', 'draws'),
    ('tex_source', 'Texture source (level 0)', 'draws'),
    ('unique_textures', 'Distinct textures (address, format, size)',
     'textures'),
    ('blend', 'Blending (enabled draws)', 'draws'),
    ('logic_op', 'Logic op (enabled draws)', 'draws'),
    ('stencil', 'Stencil test and ops (enabled draws)', 'draws'),
    ('alpha_test', 'Alpha test (enabled draws)', 'draws'),
    ('color_test', 'Color test (enabled draws)', 'draws'),
    ('depth', 'Depth test and write', 'draws'),
    ('depth_range', 'Depth clamp and range', 'draws'),
    ('lighting', 'Lighting', 'draws'),
    ('raster', 'Shading, culling, fog, dither, antialias', 'draws'),
    ('color_mask', 'Write masks (non-zero only)', 'draws'),
    ('render_targets', 'Render targets (color)', 'draws'),
    ('depth_buffers', 'Depth buffers (draws with depth test or write)',
     'draws'),
    ('scissor', 'Scissor rectangles', 'draws'),
    ('vram_reads', 'GE reads from VRAM (textures and CLUTs)', 'reads'),
    ('clut_loads', 'CLUT loads', 'loads'),
    ('transfers', 'Block transfers to VRAM', 'transfers'),
    ('cpu_vram_writes', 'CPU writes to VRAM (memcpy/memset)', 'writes'),
    ('display', 'Displayed framebuffers', 'records'),
    ('edram_translation', 'sceGeEdramSetAddrTranslation values', 'records'),
)


class Report:
  """Counters for one or more dumps."""

  def __init__(self):
    self.sections = {key: collections.Counter() for key, _, _ in SECTIONS}
    self.files = []

  def merge(self, other):
    for key, counter in other.sections.items():
      self.sections[key].update(counter)
    self.files.extend(other.files)

  def to_json(self):
    return {
        'files': self.files,
        'sections': {
            key: dict(counter.most_common())
            for key, counter in self.sections.items()
        },
    }


class _Target:
  """A color or depth buffer drawn during the dump."""

  def __init__(self, address, stride, bpp, fmt):
    self.address = address
    self.stride = stride
    self.bpp = bpp
    self.fmt = fmt
    self.height = 0

  def extent(self):
    return self.stride * self.bpp * max(self.height, 1)

  def contains(self, offset):
    return self.address <= offset < self.address + self.extent()


class Walker:
  """Replays the register stream of one dump and fills a `Report`."""

  def __init__(self, dump, label):
    self.dump = dump
    self.regs = [cmd << 24 for cmd in range(256)]
    self.report = Report()
    self.s = self.report.sections
    self.label = label
    self.color_targets = {}
    self.depth_targets = {}
    self.displayed = []
    self.framebuf_flags = {}
    self.clut_address = None
    self.memcpy_dest = None
    self.last_prim = 0
    self.textures = set()
    self.totals = collections.Counter()

  # Register helpers ---------------------------------------------------------

  def reg(self, cmd):
    return self.regs[cmd] & 0xFFFFFF

  def flag(self, cmd):
    return self.regs[cmd] & 1

  def texture_address(self, level):
    return ((self.reg(CMD_TEXADDR0 + level) & 0xFFFFF0) |
            ((self.reg(CMD_TEXBUFWIDTH0 + level) << 8) & 0x0F000000))

  def framebuffer(self):
    address = self.reg(CMD_FRAMEBUFPTR) & 0x1FFFF0
    stride = self.reg(CMD_FRAMEBUFWIDTH) & 0x7FC
    fmt = self.reg(CMD_FRAMEBUFPIXFORMAT) & 3
    return address, stride, fmt

  def depthbuffer(self):
    return self.reg(CMD_ZBUFPTR) & 0x1FFFF0, self.reg(CMD_ZBUFWIDTH) & 0x7FC

  def drawn_height(self):
    scissor_y2 = (self.reg(CMD_SCISSOR2) >> 10) & 0x3FF
    region_y2 = (self.reg(CMD_REGION2) >> 10) & 0x3FF
    return min(scissor_y2, region_y2) + 1

  # Records ------------------------------------------------------------------

  def run(self):
    s = self.s
    for index, command in enumerate(self.dump.commands):
      name = command.name
      s['dump_records'][name] += 1
      data = self.dump.data(command)
      handler = getattr(self, f'_on_{name.rstrip("01234567").lower()}', None)
      try:
        if handler is not None:
          handler(command, data)
      except ppdmp.DumpError as error:
        raise ppdmp.DumpError(f'record {index} ({name}): {error}') from error
    for _, fmt_key, _, _ in sorted(self.textures):
      s['unique_textures'][fmt_key] += 1
    self.report.files.append(self.summary())
    return self.report

  def summary(self):
    return {
        'file': self.label,
        'game_id': self.dump.game_id,
        'version': self.dump.version,
        'compression': self.dump.compression,
        'file_bytes': self.dump.file_size,
        'pushbuf_bytes': len(self.dump.pushbuf),
        'records': len(self.dump.commands),
        'draws': self.totals['draws'],
        'vertices': self.totals['vertices'],
        'render_targets': len(self.color_targets),
        'distinct_textures': len(self.textures),
    }

  def _on_init(self, command, data):
    if command.size != INIT_SIZE:
      raise ppdmp.DumpError(f'INIT holds {command.size} bytes, expected '
                            f'{INIT_SIZE}')
    words = struct.unpack_from('<512I', data, 0)
    pos = INIT_REGISTER_OFFSET
    for first, last in INIT_RANGES:
      for cmd in range(first, last + 1):
        value = words[pos]
        pos += 1
        if value >> 24 != cmd:
          raise ppdmp.DumpError(f'INIT register 0x{cmd:02X} holds a value '
                                f'for command 0x{value >> 24:02X}')
        self.regs[cmd] = value

  def _on_registers(self, command, data):
    if command.size % 4:
      raise ppdmp.DumpError('REGISTERS size is not a multiple of 4')
    for (op,) in struct.iter_unpack('<I', data):
      cmd = op >> 24
      self.s['ge_commands'][command_name(cmd)] += 1
      self.regs[cmd] = op
      if cmd == CMD_PRIM:
        self._draw(op, patch=None)
      elif cmd in (CMD_BEZIER, CMD_SPLINE):
        self._draw(op, patch=cmd)
      elif cmd == CMD_LOADCLUT:
        self._clut_load(op)
      elif cmd == CMD_TRANSFERSTART:
        self._transfer(op)

  def _on_framebuf(self, command, data):
    if command.size < 16:
      raise ppdmp.DumpError('FRAMEBUF record is shorter than its header')
    address, bufw, flags, _ = struct.unpack_from('<IiII', data, 0)
    level = command.type - 0x18
    self.framebuf_flags[level] = (address, bufw, flags)

  def _on_clutaddr(self, command, data):
    if command.size != 8:
      raise ppdmp.DumpError('CLUTADDR record must be 8 bytes')
    self.clut_address = struct.unpack_from('<II', data, 0)

  def _on_display(self, command, data):
    if command.size != 12:
      raise ppdmp.DumpError('DISPLAY record must be 12 bytes')
    address, stride, fmt = struct.unpack_from('<IiI', data, 0)
    if is_vram(address) and 0 < stride <= 1024:
      displayed = _Target(address & (VRAM_SIZE - 1), stride,
                          4 if fmt == 3 else 2, fmt)
      displayed.height = DISPLAY_HEIGHT
      self.displayed.append(displayed)
    self.s['display'][f'0x{address:08X} {_name(BUFFER_FORMATS, fmt)} '
                      f'stride={stride}'] += 1

  def _on_edramtrans(self, command, data):
    if command.size != 4:
      raise ppdmp.DumpError('EDRAMTRANS record must be 4 bytes')
    (value,) = struct.unpack_from('<I', data, 0)
    self.s['edram_translation'][f'0x{value:X}'] += 1

  def _on_memset(self, command, data):
    if command.size != 12:
      raise ppdmp.DumpError('MEMSET record must be 12 bytes')
    dest, value, size = struct.unpack_from('<IiI', data, 0)
    self._cpu_write('memset', dest, size, f' value=0x{value & 0xFF:02X}')

  def _on_memcpydest(self, command, data):
    if command.size != 4:
      raise ppdmp.DumpError('MEMCPYDEST record must be 4 bytes')
    (self.memcpy_dest,) = struct.unpack_from('<I', data, 0)

  def _on_memcpydata(self, command, data):
    del data  # Only the size matters; the pixels are not looked at.
    dest = self.memcpy_dest
    if dest is None:
      raise ppdmp.DumpError('MEMCPYDATA without MEMCPYDEST')
    self._cpu_write('memcpy', dest, command.size, '')

  def _cpu_write(self, kind, dest, size, extra):
    offset = dest & (VRAM_SIZE - 1)
    where = self._classify_vram(offset, current=None)
    self.s['cpu_vram_writes'][f'{kind} {size} bytes -> VRAM+0x{offset:06X} '
                              f'({where}){extra}'] += 1
    if where.startswith('render target'):
      self.s['features']['CPU writes into a render target'] += 1

  # VRAM classification ------------------------------------------------------

  def _classify_vram(self, offset, current):
    if current is not None and current.contains(offset):
      return 'current render target'
    for target in self.color_targets.values():
      if target.contains(offset):
        return (f'render target 0x{target.address:06X} '
                f'{BUFFER_FORMATS[target.fmt]}')
    for target in self.depth_targets.values():
      if target.contains(offset):
        return f'depth buffer 0x{target.address:06X}'
    if any(displayed.contains(offset) for displayed in self.displayed):
      return 'displayed framebuffer'
    return 'not drawn in this dump'

  # Draw state ---------------------------------------------------------------

  def _draw(self, op, patch):
    s = self.s
    features = s['features']
    vtype = self.reg(CMD_VERTEXTYPE)
    through = bool(vtype & (1 << 23))
    mode = 'through' if through else 'transform'
    if patch is None:
      count = op & 0xFFFF
      prim = (op >> 16) & 7
      if prim == 7:
        prim = self.last_prim
      self.last_prim = prim
      prim_name = PRIM_TYPES[prim]
      vertices = count
    else:
      ucount, vcount = op & 0xFF, (op >> 8) & 0xFF
      vertices = ucount * vcount
      prim_name = 'BEZIER' if patch == CMD_BEZIER else 'SPLINE'
      division = self.reg(CMD_PATCHDIVISION)
      patch_prim = _name(PATCH_PRIMS, self.reg(CMD_PATCHPRIMITIVE) & 3)
      key = (f'{prim_name} {ucount}x{vcount} div={division & 0x7F}x'
             f'{(division >> 8) & 0x7F} prim={patch_prim}')
      if patch == CMD_SPLINE:
        key += f' type={(op >> 16) & 3}x{(op >> 18) & 3}'
      if self.flag(CMD_PATCHCULLENABLE):
        key += ' patchcull'
      if self.flag(CMD_PATCHFACING):
        key += ' reversed-normals'
      s['patches'][key] += 1
      features[prim_name.lower()] += 1
    self.totals['draws'] += 1
    self.totals['vertices'] += vertices
    s['prims'][f'{prim_name} {mode}'] += 1
    s['prim_vertices'][f'{prim_name} {mode}'] += vertices
    s['vtypes'][describe_vtype(vtype)] += 1

    if patch is None:
      if prim_name in ('LINES', 'LINE_STRIP'):
        features['lines'] += 1
      elif prim_name == 'POINTS':
        features['points'] += 1
      elif prim_name == 'RECTANGLES':
        features['rectangles (sprites)'] += 1
    if through:
      features['through mode (2D)'] += 1
    if (vtype >> 9) & 3:
      features['skinning'] += 1
    if (vtype >> 18) & 7:
      features['morphing'] += 1
    if (vtype >> 11) & 3:
      features[f'index buffer {("", "8", "16", "32")[(vtype >> 11) & 3]}'
               '-bit'] += 1

    fb_address, fb_stride, fb_fmt = self.framebuffer()
    bpp = 4 if fb_fmt == 3 else 2
    target = self.color_targets.get(fb_address)
    if target is None or target.stride != fb_stride or target.fmt != fb_fmt:
      target = _Target(fb_address, fb_stride, bpp, fb_fmt)
      self.color_targets[fb_address] = target
    target.height = max(target.height, self.drawn_height())
    s['render_targets'][f'0x{fb_address:06X} {BUFFER_FORMATS[fb_fmt]} '
                        f'stride={fb_stride}'] += 1
    if fb_fmt != 3:
      features[f'{BUFFER_FORMATS[fb_fmt]} framebuffer'] += 1
    scissor1, scissor2 = self.reg(CMD_SCISSOR1), self.reg(CMD_SCISSOR2)
    s['scissor'][f'{scissor1 & 0x3FF},{(scissor1 >> 10) & 0x3FF}-'
                 f'{scissor2 & 0x3FF},{(scissor2 >> 10) & 0x3FF}'] += 1

    clear = self.flag(CMD_CLEARMODE)
    if clear:
      mask = self.reg(CMD_CLEARMODE)
      channels = [name for bit, name in ((0x100, 'color'),
                                         (0x200, 'alpha/stencil'),
                                         (0x400, 'depth')) if mask & bit]
      s['clears'][f'{"+".join(channels) or "nothing"} '
                  f'{BUFFER_FORMATS[fb_fmt]}'] += 1
      features['clear mode'] += 1
      if mask & 0x400:
        self._depth_target()
      return

    self._texture_state(target, vtype)
    self._pixel_state(fb_fmt)

  def _depth_target(self):
    z_address, z_stride = self.depthbuffer()
    depth = self.depth_targets.get(z_address)
    if depth is None or depth.stride != z_stride:
      depth = _Target(z_address, z_stride, 2, None)
      self.depth_targets[z_address] = depth
    depth.height = max(depth.height, self.drawn_height())
    self.s['depth_buffers'][f'0x{z_address:06X} stride={z_stride}'] += 1

  def _texture_state(self, current_target, vtype):
    s = self.s
    features = s['features']
    textured = self.flag(CMD_TEXTUREMAPENABLE)
    if not textured:
      return
    features['texturing'] += 1
    fmt = self.reg(CMD_TEXFORMAT) & 0xF
    fmt_name = _name(TEXTURE_FORMATS, fmt)
    clut_fmt = self.reg(CMD_CLUTFORMAT)
    if 4 <= fmt <= 7:
      fmt_key = f'{fmt_name} (palette {CLUT_FORMATS[clut_fmt & 3]})'
      simple = (clut_fmt & ~3 & 0xFFFFFF) == 0x00FF00
      s['tex_clut'][
          'simple' if simple else
          f'shift={(clut_fmt >> 2) & 0x1F} mask=0x{(clut_fmt >> 8) & 0xFF:02X} '
          f'start={((clut_fmt >> 16) & 0x1F) << 4}'] += 1
      if not simple:
        features['CLUT with shift/mask/offset'] += 1
    else:
      fmt_key = fmt_name
    s['tex_format'][fmt_key] += 1
    features[f'texture {fmt_name}'] += 1

    size = self.reg(CMD_TEXSIZE0)
    width, height = 1 << (size & 0xF), 1 << ((size >> 8) & 0xF)
    s['tex_size'][f'{width}x{height}'] += 1
    texmode = self.reg(CMD_TEXMODE)
    texfilter = self.reg(CMD_TEXFILTER)
    texlevel = self.reg(CMD_TEXLEVEL)
    max_level = (texmode >> 16) & 7
    swizzled = texmode & 1
    mode_key = f'{"swizzled" if swizzled else "linear"} levels={max_level + 1}'
    if max_level:
      bias = (texlevel >> 16) & 0xFF
      bias -= (bias & 0x80) << 1  # Signed 8-bit LOD bias (1/16 steps).
      mode_key += f' lod={TEX_LEVEL_MODES[texlevel & 3]} bias={bias}'
      if not texmode & 0x100:
        mode_key += ' shared-clut'
      features['mipmaps'] += 1
    s['tex_mode'][mode_key] += 1
    if swizzled:
      features['swizzled textures'] += 1
    minify = TEX_FILTERS.get(texfilter & 7, f'INVALID{texfilter & 7}')
    magnify = 'LINEAR' if (texfilter >> 8) & 1 else 'NEAREST'
    s['tex_filter'][f'min={minify} mag={magnify}'] += 1
    wrap = self.reg(CMD_TEXWRAP)
    s['tex_wrap'][f's={"clamp" if wrap & 1 else "repeat"} '
                  f't={"clamp" if (wrap >> 8) & 1 else "repeat"}'] += 1
    func = self.reg(CMD_TEXFUNC)
    func_key = (f'{TEX_FUNCS[func & 7]} '
                f'{"rgba" if func & 0x100 else "rgb"}')
    if func & 0x10000:
      func_key += ' x2'
      features['color doubling'] += 1
    s['tex_func'][func_key] += 1
    features[f'texfunc {TEX_FUNCS[func & 7]}'] += 1
    mapmode = self.reg(CMD_TEXMAPMODE)
    uv_key = UV_GEN_MODES[mapmode & 3]
    if mapmode & 3 == 1:
      uv_key += f' proj={UV_PROJ_MODES[(mapmode >> 8) & 3]}'
      features['texture matrix (UV gen)'] += 1
    elif mapmode & 3 == 2:
      features['environment map (UV gen)'] += 1
    if not vtype & 3 and mapmode & 3 == 0:
      uv_key += ' (no tc in vtype)'
    s['tex_uvgen'][uv_key] += 1

    address = self.texture_address(0)
    self.textures.add((address, fmt_key, width, height))
    if is_vram(address):
      offset = address & (VRAM_SIZE - 1)
      mirror = (address >> 21) & 3
      where = self._classify_vram(offset, current_target)
      extra = ''
      flags = self.framebuf_flags.get(0)
      if flags is not None and flags[0] == address:
        extra = (f' recorder-flags={"T" if flags[2] & 1 else "-"}'
                 f'{"D" if flags[2] & 4 else "-"}')
      s['tex_source'][f'VRAM: {where}'] += 1
      s['vram_reads'][f'texture {fmt_name} {width}x{height} @ VRAM+'
                      f'0x{offset:06X} mirror={mirror}: {where}{extra}'] += 1
      features['texture from VRAM'] += 1
      if where == 'current render target':
        features['self-texturing (texture = current target)'] += 1
      elif where.startswith('render target'):
        features['render-to-texture'] += 1
      elif where.startswith('depth buffer'):
        features['depth buffer as texture'] += 1
      elif where == 'displayed framebuffer':
        features['texture from displayed framebuffer'] += 1
    else:
      s['tex_source']['RAM'] += 1

  def _pixel_state(self, fb_fmt):
    s = self.s
    features = s['features']
    if self.flag(CMD_ALPHABLENDENABLE):
      blend = self.reg(CMD_BLENDMODE)
      src, dst, eq = blend & 0xF, (blend >> 4) & 0xF, (blend >> 8) & 7
      src_name = BLEND_SRC[src] if src < 10 else 'FIXA'
      dst_name = BLEND_DST[dst] if dst < 10 else 'FIXB'
      key = f'{_name(BLEND_EQUATIONS, eq)}({src_name}, {dst_name})'
      if src >= 10:
        key += f' fixA=0x{self.reg(CMD_BLENDFIXEDA):06X}'
      if dst >= 10:
        key += f' fixB=0x{self.reg(CMD_BLENDFIXEDB):06X}'
      s['blend'][key] += 1
      features['blending'] += 1
      if eq >= 3:
        features['blend MIN/MAX/ABSDIFF'] += 1
      if src >= 10 or dst >= 10:
        features['blend fixed colors'] += 1
      if 6 <= src <= 9 or 6 <= dst <= 9:
        features['blend doubled factors'] += 1
      if src in (4, 5) or dst in (4, 5) or 8 <= src <= 9 or 8 <= dst <= 9:
        features['blend reads dst alpha'] += 1
    if self.flag(CMD_LOGICOPENABLE):
      s['logic_op'][LOGIC_OPS[self.reg(CMD_LOGICOP) & 0xF]] += 1
      features['logic op'] += 1
    if self.flag(CMD_STENCILTESTENABLE):
      test, ops = self.reg(CMD_STENCILTEST), self.reg(CMD_STENCILOP)
      s['stencil'][f'{COMPARISONS[test & 7]} ref=0x{(test >> 8) & 0xFF:02X} '
                   f'mask=0x{(test >> 16) & 0xFF:02X} '
                   f'sfail={STENCIL_OPS[ops & 7]} '
                   f'zfail={STENCIL_OPS[(ops >> 8) & 7]} '
                   f'zpass={STENCIL_OPS[(ops >> 16) & 7]} '
                   f'fb={BUFFER_FORMATS[fb_fmt]}'] += 1
      features['stencil test'] += 1
      if any((ops >> shift) & 7 for shift in (0, 8, 16)):
        features['stencil write'] += 1
    if self.flag(CMD_ALPHATESTENABLE):
      test = self.reg(CMD_ALPHATEST)
      s['alpha_test'][f'{COMPARISONS[test & 7]} ref=0x{(test >> 8) & 0xFF:02X} '
                      f'mask=0x{(test >> 16) & 0xFF:02X}'] += 1
      features['alpha test'] += 1
    if self.flag(CMD_COLORTESTENABLE):
      s['color_test'][f'{COMPARISONS[self.reg(CMD_COLORTEST) & 3]} '
                      f'ref=0x{self.reg(CMD_COLORREF):06X} '
                      f'mask=0x{self.reg(CMD_COLORTESTMASK):06X}'] += 1
      features['color test'] += 1
    depth_test = self.flag(CMD_ZTESTENABLE)
    depth_write = not self.flag(CMD_ZWRITEDISABLE)
    if depth_test:
      key = f'test={COMPARISONS[self.reg(CMD_ZTEST) & 7]}'
      key += f' write={"on" if depth_write else "off"}'
      features['depth test'] += 1
      self._depth_target()
    else:
      key = 'test=off'
    s['depth'][key] += 1
    minz, maxz = self.reg(CMD_MINZ) & 0xFFFF, self.reg(CMD_MAXZ) & 0xFFFF
    clamp = self.flag(CMD_DEPTHCLAMPENABLE)
    s['depth_range'][f'clamp={"on" if clamp else "off"} '
                     f'minz=0x{minz:04X} maxz=0x{maxz:04X}'] += 1
    if clamp:
      features['depth clamp'] += 1
    if self.flag(CMD_LIGHTINGENABLE):
      lights = []
      for chan in range(4):
        if self.flag(CMD_LIGHTENABLE0 + chan):
          light = self.reg(CMD_LIGHTTYPE0 + chan)
          lights.append(f'{LIGHT_TYPES[(light >> 8) & 3]}/'
                        f'{LIGHT_COMPUTATIONS[light & 3]}')
      s['lighting'][f'on: {", ".join(lights) or "no lights"}'] += 1
      features['lighting'] += 1
    else:
      s['lighting']['off'] += 1
    raster = [f'shade={"gouraud" if self.flag(CMD_SHADEMODE) else "flat"}']
    if self.flag(CMD_CULLFACEENABLE):
      raster.append(f'cull={"CCW" if self.flag(CMD_CULL) else "CW"}')
    if self.flag(CMD_FOGENABLE):
      raster.append('fog')
      features['fog'] += 1
    if self.flag(CMD_DITHERENABLE):
      raster.append('dither')
      features['dither'] += 1
    if self.flag(CMD_ANTIALIASENABLE):
      raster.append('antialias')
      features['antialias'] += 1
    s['raster'][' '.join(raster)] += 1
    rgb_mask, alpha_mask = (self.reg(CMD_MASKRGB),
                            self.reg(CMD_MASKALPHA) & 0xFF)
    if rgb_mask or alpha_mask:
      s['color_mask'][f'rgb=0x{rgb_mask:06X} alpha=0x{alpha_mask:02X}'] += 1
      features['color/alpha write mask'] += 1

  # Other GE operations ------------------------------------------------------

  def _clut_load(self, op):
    blocks = 0x40 if (op & 0x7F) == 0x40 else op & 0x3F
    address = ((self.reg(0xB0) & 0xFFFFF0) |
               ((self.reg(0xB1) << 8) & 0x0F000000))
    source = 'RAM'
    if is_vram(address):
      offset = address & (VRAM_SIZE - 1)
      current = self.color_targets.get(self.framebuffer()[0])
      where = self._classify_vram(offset, current)
      source = f'VRAM ({where})'
      extra = ''
      if self.clut_address is not None and self.clut_address[0] == address:
        flags = self.clut_address[1]
        extra = (f' recorder-flags={"T" if flags & 1 else "-"}'
                 f'{"D" if flags & 4 else "-"}')
      self.s['vram_reads'][f'CLUT {blocks * 32} bytes @ VRAM+0x{offset:06X}: '
                           f'{where}{extra}'] += 1
      self.s['features']['CLUT from VRAM'] += 1
    self.clut_address = None
    self.s['clut_loads'][f'{blocks * 32} bytes from {source}'] += 1

  def _transfer(self, op):
    src = ((self.reg(CMD_TRANSFERSRC) & 0xFFFFF0) |
           ((self.reg(CMD_TRANSFERSRCW) & 0xFF0000) << 8))
    dst = ((self.reg(CMD_TRANSFERDST) & 0xFFFFF0) |
           ((self.reg(CMD_TRANSFERDSTW) & 0xFF0000) << 8))
    size = self.reg(CMD_TRANSFERSIZE)
    width, height = (size & 0x3FF) + 1, ((size >> 10) & 0x3FF) + 1
    bpp = 4 if op & 1 else 2
    if is_vram(src):
      where = self._classify_vram(src & (VRAM_SIZE - 1), current=None)
      source = f'VRAM ({where})'
      self.s['features']['block transfer VRAM->VRAM'] += 1
    else:
      source = 'RAM'
      self.s['features']['block transfer RAM->VRAM'] += 1
    target = self._classify_vram(dst & (VRAM_SIZE - 1), current=None)
    self.s['transfers'][f'{source} -> VRAM ({target}) {width}x{height} '
                        f'bpp={bpp}'] += 1


def analyze(dump, label):
  """Returns the `Report` for one parsed dump."""
  return Walker(dump, label).run()


def _format_text(report, title):
  lines = [f'== {title} ==']
  for entry in report.files:
    lines.append('  '.join(f'{key}={value}' for key, value in entry.items()))
  for key, section_title, unit in SECTIONS:
    counter = report.sections[key]
    if not counter:
      continue
    lines.append('')
    lines.append(f'-- {section_title} [{unit}] --')
    for name, count in counter.most_common():
      lines.append(f'{count:>9}  {name}')
  return '\n'.join(lines)


def _escape(text):
  return str(text).replace('|', '\\|')


def _format_markdown(report, title):
  lines = [f'## {title}', '']
  if report.files:
    keys = list(report.files[0])
    lines.append('| ' + ' | '.join(keys) + ' |')
    lines.append('|' + '---|' * len(keys))
    for entry in report.files:
      lines.append('| ' + ' | '.join(_escape(entry[k]) for k in keys) + ' |')
  for key, section_title, unit in SECTIONS:
    counter = report.sections[key]
    if not counter:
      continue
    lines += ['', f'### {section_title}', '', f'| {unit} | value |',
              '|---:|---|']
    for name, count in counter.most_common():
      lines.append(f'| {count} | `{_escape(name)}` |')
  return '\n'.join(lines)


def _collect_paths(arguments):
  paths = []
  for argument in arguments:
    path = pathlib.Path(argument)
    if path.is_dir():
      paths.extend(sorted(path.glob('*.ppdmp')))
    else:
      paths.append(path)
  return paths


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__.split('\n\n', maxsplit=1)[0])
  parser.add_argument('paths', nargs='+',
                      help='.ppdmp files or directories that contain them')
  parser.add_argument('--format', choices=('text', 'markdown', 'json'),
                      default='text')
  parser.add_argument('--per-file', action='store_true',
                      help='also print one report per dump')
  parser.add_argument('--pure-python', action='store_true',
                      help='ignore native zstd modules (zstandard, '
                      'compression.zstd)')
  parser.add_argument('--max-pushbuf-mib', type=int, default=1024,
                      help='reject dumps whose pushbuf is larger (default '
                      '%(default)s)')
  args = parser.parse_args(argv)

  paths = _collect_paths(args.paths)
  if not paths:
    print('error: no .ppdmp files found', file=sys.stderr)
    return 2
  total = Report()
  per_file = []
  failed = 0
  for path in paths:
    try:
      dump = ppdmp.load(path, max_pushbuf=args.max_pushbuf_mib << 20,
                        force_pure=args.pure_python)
      report = analyze(dump, path.name)
    except (OSError, ppdmp.DumpError) as error:
      print(f'error: {path}: {error}', file=sys.stderr)
      failed += 1
      continue
    total.merge(report)
    per_file.append((path.name, report))

  if args.format == 'json':
    output = {'total': total.to_json()}
    if args.per_file:
      output['per_file'] = {name: rep.to_json() for name, rep in per_file}
    print(json.dumps(output, indent=1, sort_keys=False))
  else:
    formatter = _format_markdown if args.format == 'markdown' else _format_text
    chunks = [formatter(total, f'All dumps ({len(per_file)})')]
    if args.per_file:
      chunks += [formatter(rep, name) for name, rep in per_file]
    print('\n\n'.join(chunks))
  return 1 if failed else 0


if __name__ == '__main__':
  sys.exit(main())
