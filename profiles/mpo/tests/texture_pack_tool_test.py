#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests profiles/mpo/tools/texture_pack.py (F26b) with synthetic data only.

The dumps are made-up keys with made-up images (gradients and checkers, no
game art), and the scalers are fakes written here: a per-image command and a
stand-in for realesrgan-ncnn-vulkan on a folder. Both drop the alpha channel
and repeat pixels, as a real scaler would drop it and invent detail.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import struct
import sys
import tempfile
import textwrap
import unittest
import zlib

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load(name):
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


texture_pack = _load('texture_pack')

_WALL = '00000000000a010412345678'
_ICON = '0000000009f357c00cdcc7e2'
_LEAF = '000000000005010489abcdef'
_GHOST = '00000000000a0104aaaaaaaa'  # In dump.csv, without a PNG.
_OLD = '0000000000010101bbbbbbbb'  # A PNG without a row.

# The text DumpIndex writes in mpo_texreplace_test (TestDumpIndex).
_CSV_FROM_CPP = (
    'key,width,height,format,memory,first_stage,stages,uses,frames\n'
    '00000000000a010412345678,128,64,DXT5,vram,st02,st02,1,1\n'
    '0000000009f357c00cdcc7e2,32,16,CLUT4,ram+vram,,st04vo;briefing,5,3\n')

_FAKE_COMMAND = textwrap.dedent('''\
    # SPDX-License-Identifier: GPL-3.0-or-later
    """A fake scaler: tool input output scale nearest|dim|small|fail."""
    import importlib.util
    import sys
    tool, src, dst, scale, mode = sys.argv[1:6]
    spec = importlib.util.spec_from_file_location('texture_pack', tool)
    tp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tp)
    if mode == 'fail':
      print('fake scaler: out of memory')
      sys.exit(3)
    scale = int(scale)
    w, h, rgba = tp.load_png(src)
    rgb = bytearray(w * h * 3)
    for c in range(3):
      rgb[c::3] = rgba[c::4]
    if mode == 'dim':
      rgb = bytes(v * 3 // 4 for v in rgb)
    if mode == 'small':
      scale -= 1
    tp.save_png(dst, w * scale, h * scale, tp.nearest(w, h, rgb, 3, scale), 3)
    ''')

_FAKE_REALESRGAN = textwrap.dedent('''\
    # SPDX-License-Identifier: GPL-3.0-or-later
    """A stand-in for realesrgan-ncnn-vulkan: -i, -o, -s, -n, -f, -m."""
    import importlib.util
    import os
    import sys
    tool, log = sys.argv[1:3]
    args = sys.argv[3:]
    with open(log, 'w') as out:
      out.write(' '.join(args))
    opts = dict(zip(args[0::2], args[1::2]))
    spec = importlib.util.spec_from_file_location('texture_pack', tool)
    tp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tp)
    scale = int(opts['-s'])
    for name in os.listdir(opts['-i']):
      w, h, rgba = tp.load_png(os.path.join(opts['-i'], name))
      rgb = bytearray(w * h * 3)
      for c in range(3):
        rgb[c::3] = rgba[c::4]
      tp.save_png(os.path.join(opts['-o'], name), w * scale, h * scale,
                  tp.nearest(w, h, rgb, 3, scale), 3)
    ''')


def _gradient(width, height, alpha=None):
  """RGBA bytes; alpha(x, y) gives the alpha (opaque by default)."""
  out = bytearray()
  for y in range(height):
    for x in range(width):
      a = 255 if alpha is None else alpha(x, y)
      out += bytes(((x * 7) & 0xFF, (y * 11) & 0xFF, (x * y) & 0xFF, a))
  return bytes(out)


def _chunk(kind, body):
  return (struct.pack('>I', len(body)) + kind + body +
          struct.pack('>I', zlib.crc32(kind + body)))


def _raw_png(width, height, depth, color, rows, extra=b''):
  """A PNG from already filtered rows (each with its filter byte)."""
  return (b'\x89PNG\r\n\x1a\n' +
          _chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color,
                                      0, 0, 0)) +
          extra + _chunk(b'IDAT', zlib.compress(rows)) + _chunk(b'IEND', b''))


def _filtered(pixels, width, height, bpp, filters):
  stride = width * bpp
  rows = bytearray()
  prev = bytes(stride)
  for y in range(height):
    line = pixels[y * stride:(y + 1) * stride]
    kind = filters[y % len(filters)]
    out = bytearray()
    for i in range(stride):
      left = line[i - bpp] if i >= bpp else 0
      up = prev[i]
      corner = prev[i - bpp] if i >= bpp else 0
      if kind == 0:
        pred = 0
      elif kind == 1:
        pred = left
      elif kind == 2:
        pred = up
      elif kind == 3:
        pred = (left + up) // 2
      else:
        pred = texture_pack._paeth(left, up, corner)
      out.append((line[i] - pred) & 0xFF)
    rows += bytes((kind,)) + out
    prev = line
  return bytes(rows)


class PngTest(unittest.TestCase):

  def test_every_filter_round_trips(self):
    pixels = _gradient(13, 9, lambda x, y: (x * 19 + y) & 0xFF)
    data = _raw_png(13, 9, 8, 6, _filtered(pixels, 13, 9, 4, (0, 1, 2, 3, 4)))
    self.assertEqual(texture_pack.read_png(data), (13, 9, pixels))
    self.assertEqual(texture_pack.check_png_data(data), (13, 9))

  def test_encode_and_read(self):
    pixels = _gradient(17, 5, lambda x, y: x * 15)
    data = texture_pack.encode_png(17, 5, pixels)
    self.assertEqual(texture_pack.read_png(data), (17, 5, pixels))
    rgb = bytes(b for i, b in enumerate(pixels) if i % 4 != 3)
    self.assertEqual(texture_pack.read_png(
        texture_pack.encode_png(17, 5, rgb, 3))[2],
                     _gradient(17, 5))

  def test_other_colour_types(self):
    # 8-bit grey.
    grey = bytes(range(0, 240, 15))
    data = _raw_png(4, 4, 8, 0, _filtered(grey, 4, 4, 1, (1,)))
    _, _, rgba = texture_pack.read_png(data)
    self.assertEqual(rgba[0:8], bytes((0, 0, 0, 255, 15, 15, 15, 255)))
    # 16-bit grey + alpha: the high bytes.
    ga = struct.pack('>4H', 0x1234, 0xFF00, 0xABCD, 0x0001)
    data = _raw_png(2, 1, 16, 4, _filtered(ga, 2, 1, 4, (0,)))
    self.assertEqual(texture_pack.read_png(data)[2],
                     bytes((0x12, 0x12, 0x12, 0xFF, 0xAB, 0xAB, 0xAB, 0)))
    # 4-bit palette with transparency.
    plte = _chunk(b'PLTE', bytes((10, 20, 30, 40, 50, 60)))
    trns = _chunk(b'tRNS', bytes((0,)))
    rows = b'\x00' + bytes((0x01, 0x10))  # Indices 0 1 1 (padded).
    data = _raw_png(3, 1, 4, 3, rows, plte + trns)
    self.assertEqual(texture_pack.read_png(data)[2],
                     bytes((10, 20, 30, 0, 40, 50, 60, 255, 40, 50, 60, 255)))

  def test_bad_files(self):
    good = texture_pack.encode_png(4, 4, _gradient(4, 4))
    bad_crc = bytearray(good)
    bad_crc[-20] ^= 0xFF  # Inside the IDAT.
    cases = {
        'not a PNG': b'GIF89a',
        'truncated': good[:40],
        'bad CRC': bytes(bad_crc),
        'huge': _raw_png(100000, 1, 8, 6, b''),
        'short data': _raw_png(4, 4, 8, 6, b'\x00' * 10),
    }
    for label, data in cases.items():
      with self.subTest(label):
        with self.assertRaises(texture_pack.PackError):
          texture_pack.read_png(data)
        with self.assertRaises(texture_pack.PackError):
          texture_pack.check_png_data(data)


class DumpCsvTest(unittest.TestCase):

  def test_reads_what_the_runtime_writes(self):
    textures = texture_pack.parse_dump_csv(_CSV_FROM_CPP)
    icon = textures[_ICON]
    self.assertEqual((icon.width, icon.height, icon.format, icon.memory),
                     (32, 16, 'CLUT4', 'ram+vram'))
    self.assertEqual(icon.first_stage, '')
    self.assertEqual(icon.stages, ('st04vo', 'briefing'))
    self.assertEqual((icon.uses, icon.frames), (5, 3))
    crlf = _CSV_FROM_CPP.replace('\n', '\r\n')
    self.assertEqual(texture_pack.parse_dump_csv(crlf), textures)

  def test_refuses_what_the_runtime_refuses(self):
    header = texture_pack.DUMP_HEADER + '\n'
    rows = [
        f'{_ICON},32,16,CLUT4,ram,,,1\n',
        f'{_ICON.upper()},32,16,CLUT4,ram,,,1,1\n',
        f'{_ICON[:-1]},32,16,CLUT4,ram,,,1,1\n',
        f'{_ICON},0,16,CLUT4,ram,,,1,1\n',
        f'{_ICON},32,4097,CLUT4,ram,,,1,1\n',
        f'{_ICON},32,16,CLUT2,ram,,,1,1\n',
        f'{_ICON},32,16,CLUT4,edram,,,1,1\n',
        f'{_ICON},32,16,CLUT4,ram,a b,,1,1\n',
        f'{_ICON},32,16,CLUT4,ram,,st02;;st03,1,1\n',
        f'{_ICON},32,16,CLUT4,ram,,st02;st02,1,1\n',
        f'{_ICON},32,16,CLUT4,ram,,,-1,1\n',
        f'{_ICON},32,16,CLUT4,ram,,,99999999999999999999,1\n',
        f'{_ICON},32,16,CLUT4,ram,,,1,1\n' * 2,
    ]
    for row in rows:
      with self.subTest(row):
        with self.assertRaises(texture_pack.PackError):
          texture_pack.parse_dump_csv(header + row)
    with self.assertRaises(texture_pack.PackError):
      texture_pack.parse_dump_csv('key,width\n')
    self.assertEqual(texture_pack.parse_dump_csv(''), {})


class _DumpCase(unittest.TestCase):
  """A dump folder with four PNGs and a dump.csv."""

  def setUp(self):
    self._tmp = tempfile.TemporaryDirectory()
    self.root = self._tmp.name
    self.dump = os.path.join(self.root, 'dump')
    os.makedirs(self.dump)
    self.images = {
        _WALL: (16, 8, _gradient(16, 8)),
        # Cut-out: alpha only 0 and 255.
        _ICON: (8, 4, _gradient(8, 4, lambda x, y: 255 if x % 3 else 0)),
        # Soft alpha.
        _LEAF: (4, 4, _gradient(4, 4, lambda x, y: 128 if y < 2 else 0)),
        _OLD: (2, 2, _gradient(2, 2)),
    }
    for key, (w, h, rgba) in self.images.items():
      texture_pack.save_png(os.path.join(self.dump, f'{key}.png'), w, h, rgba)
    with open(os.path.join(self.dump, 'dump.csv'), 'w', newline='\n') as out:
      out.write(texture_pack.DUMP_HEADER + '\n')
      out.write(f'{_WALL},16,8,8888,vram,st04vo,st04vo;st02,9000,600\n')
      out.write(f'{_ICON},8,4,CLUT4,ram,,init;title;st04vo,300,300\n')
      out.write(f'{_LEAF},4,4,CLUT8,vram,st02,st02,4000,900\n')
      out.write(f'{_GHOST},16,8,DXT1,vram,title,title,7,7\n')
    with open(os.path.join(self.dump, 'textures.ini'), 'w') as out:
      out.write(texture_pack.DEFAULT_TEXTURES_INI)

  def tearDown(self):
    self._tmp.cleanup()


class ReportTest(_DumpCase):

  def _report(self, **kwargs):
    lines = []
    chosen = texture_pack.report(self.dump, out=lines.append, **kwargs)
    return [t.key for t in chosen], lines

  def test_by_use(self):
    keys, lines = self._report()
    self.assertEqual(keys, [_WALL, _LEAF, _ICON, _GHOST, _OLD])
    self.assertIn('(no PNG)', next(l for l in lines if l.startswith(_GHOST)))
    old = next(l for l in lines if l.startswith(_OLD))
    self.assertIn('?', old)  # No row: no uses.
    self.assertTrue(lines[-1].startswith('5 of 5 textures, 13307 uses, 3 in '
                                         'VRAM'))

  def test_other_orders_and_filters(self):
    self.assertEqual(self._report(sort='frames')[0],
                     [_LEAF, _WALL, _ICON, _GHOST, _OLD])
    self.assertEqual(self._report(sort='size')[0][:2], [_WALL, _GHOST])
    # By stage, in the story's order (build_order.toml): none, title, st04vo,
    # st02.
    keys, lines = self._report(sort='stage')
    self.assertEqual(keys, [_ICON, _OLD, _GHOST, _WALL, _LEAF])
    groups = [l for l in lines if l.startswith('# ')]
    self.assertEqual(groups[0], '# (before any stage): 2 textures, 300 uses')
    self.assertTrue(groups[1].startswith('# title:'))
    # Drawn in a stage (not only first): st02 has the wall and the leaf.
    self.assertEqual(self._report(stage='st02')[0], [_WALL, _LEAF])
    self.assertEqual(self._report(limit=2)[0], [_WALL, _LEAF])
    keys, lines = self._report(keys_only=True, limit=1)
    self.assertEqual(lines, [_WALL])

  def test_without_dump_csv(self):
    os.remove(os.path.join(self.dump, 'dump.csv'))
    keys, lines = self._report()
    self.assertEqual(sorted(keys), sorted([_WALL, _ICON, _LEAF, _OLD]))
    self.assertTrue(lines[0].startswith('note:'))

  def test_bad_dump_csv(self):
    with open(os.path.join(self.dump, 'dump.csv'), 'a') as out:
      out.write('garbage\n')
    with self.assertRaisesRegex(texture_pack.PackError, 'dump.csv: line 6'):
      texture_pack.load_dump(self.dump)

  def test_command_line(self):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
      self.assertEqual(texture_pack.main(['report', self.dump, '--keys-only',
                                          '--stage', 'st04vo']), 0)
    self.assertEqual(out.getvalue().split(), [_WALL, _ICON])


class UpscaleTest(_DumpCase):

  def setUp(self):
    super().setUp()
    self.pack = os.path.join(self.root, 'pack')
    self.scripts = os.path.join(self.root, 'scripts')
    os.makedirs(self.scripts)
    self.fake = os.path.join(self.scripts, 'fake_scaler.py')
    with open(self.fake, 'w') as out:
      out.write(_FAKE_COMMAND)
    self.fake_esrgan = os.path.join(self.scripts, 'fake_realesrgan.py')
    with open(self.fake_esrgan, 'w') as out:
      out.write(_FAKE_REALESRGAN)
    self.logs = []

  def _command(self, mode='nearest'):
    return [sys.executable, '-I', self.fake, str(_TOOLS / 'texture_pack.py'),
            '{input}', '{output}', '{scale}', mode]

  def _upscale(self, **kwargs):
    kwargs.setdefault('command', self._command())
    return texture_pack.upscale(self.dump, self.pack, log=self.logs.append,
                                **kwargs)

  def _pack_image(self, key):
    return texture_pack.load_png(os.path.join(self.pack, f'{key}.png'))

  def test_scales_and_keeps_alpha_and_names(self):
    result = self._upscale(scale=2)
    self.assertEqual(sorted(result.written), sorted([_WALL, _ICON, _LEAF,
                                                     _OLD]))
    self.assertEqual(result.failed, [])
    for key, (w, h, rgba) in self.images.items():
      with self.subTest(key):
        pw, ph, prgba = self._pack_image(key)
        self.assertEqual((pw, ph), (w * 2, h * 2))
        # The fake repeats pixels: the result is the nearest upscale, alpha
        # included, although the scaler never saw an alpha channel.
        self.assertEqual(prgba, texture_pack.nearest(w, h, rgba, 4, 2))
    self.assertEqual(
        sorted(os.listdir(self.pack)),
        sorted([f'{k}.png' for k in self.images] + ['textures.ini']))
    with open(os.path.join(self.pack, 'textures.ini')) as ini:
      self.assertEqual(ini.read(), texture_pack.DEFAULT_TEXTURES_INI)
    self.assertEqual(texture_pack.check(self.pack, self.dump).errors, [])

  def test_cut_out_alpha_stays_binary(self):
    self._upscale(scale=3, command=self._command('dim'))
    _, _, icon = self._pack_image(_ICON)
    self.assertEqual(set(icon[3::4]), {0, 255})
    w, h, rgba = self.images[_ICON]
    self.assertEqual(icon[3::4], texture_pack.nearest(w, h, rgba, 4, 3)[3::4])
    _, _, leaf = self._pack_image(_LEAF)
    self.assertEqual(set(leaf[3::4]), {96, 0})  # 128 dimmed by the scaler.
    _, _, wall = self._pack_image(_WALL)
    self.assertEqual(set(wall[3::4]), {255})
    # The colour is the scaler's: pixel (3, 0) of x3 is pixel (1, 0), dimmed.
    _, _, rgba = self.images[_WALL]
    self.assertEqual(wall[12:15], bytes(v * 3 // 4 for v in rgba[4:7]))

  def test_alpha_nearest(self):
    self._upscale(scale=2, command=self._command('dim'), alpha='nearest')
    _, _, leaf = self._pack_image(_LEAF)
    w, h, rgba = self.images[_LEAF]
    self.assertEqual(leaf[3::4], texture_pack.nearest(w, h, rgba, 4, 2)[3::4])

  def test_selection_skip_and_overwrite(self):
    result = self._upscale(scale=2, top=1)
    self.assertEqual(result.written, [_WALL])
    result = self._upscale(scale=2, stage='st02')
    self.assertEqual(result.written, [_LEAF])
    self.assertEqual(result.skipped, [_WALL])
    result = self._upscale(scale=2, min_uses=1000, overwrite=True)
    self.assertEqual(sorted(result.written), sorted([_WALL, _LEAF]))
    result = self._upscale(scale=2, keys=[_ICON, _GHOST])
    self.assertEqual(result.written, [_ICON])
    self.assertTrue(any(_GHOST in l for l in self.logs))
    with self.assertRaises(texture_pack.PackError):
      self._upscale(scale=2, keys=['../../etc/passwd'])
    result = self._upscale(scale=8, keys=[_WALL], overwrite=True)
    self.assertEqual(result.written, [_WALL])
    self.assertEqual(self._pack_image(_WALL)[:2], (128, 64))
    with self.assertRaises(texture_pack.PackError):
      self._upscale(scale=9)

  def test_too_large_for_the_runtime(self):
    # 1100 x 4 > 4096: skipped before the scaler runs.
    with open(os.path.join(self.dump, 'dump.csv'), 'a', newline='\n') as out:
      out.write(f'{_OLD[:-8]}cccccccc,1100,1,8888,ram,,,1,1\n')
    texture_pack.save_png(os.path.join(self.dump, f'{_OLD[:-8]}cccccccc.png'),
                          1100, 1, _gradient(1100, 1))
    result = self._upscale(scale=4, keys=[f'{_OLD[:-8]}cccccccc'])
    self.assertEqual((result.written, len(result.skipped)), ([], 1))
    self.assertTrue(any('larger than 4096' in l for l in self.logs))

  def test_scaler_failures(self):
    result = self._upscale(scale=2, command=self._command('small'))
    self.assertEqual(result.written, [])
    self.assertEqual(len(result.failed), 4)
    self.assertTrue(any('not 32x16' in l for l in self.logs))
    self.assertFalse(os.path.exists(os.path.join(self.pack, f'{_WALL}.png')))
    result = self._upscale(scale=2, command=self._command('fail'))
    self.assertEqual(len(result.failed), 4)
    self.assertTrue(any('out of memory' in l for l in self.logs))
    with self.assertRaises(texture_pack.PackError):
      self._upscale(scale=2, command=['tool', '{output}'])
    with self.assertRaises(texture_pack.PackError):
      texture_pack.upscale(self.dump, self.dump, command=self._command())
    # No work folder is left behind.
    self.assertEqual([n for n in os.listdir(self.pack) if n.startswith('.')],
                     [])

  def test_realesrgan_on_a_folder(self):
    log = os.path.join(self.root, 'argv.txt')
    prefix = [sys.executable, '-I', self.fake_esrgan,
              str(_TOOLS / 'texture_pack.py'), log]
    result = self._upscale(scale=4, command=None, realesrgan=prefix,
                           model='realesr-animevideov3', models_dir='models')
    self.assertEqual(len(result.written), 4)
    with open(log) as argv:
      words = argv.read().split()
    self.assertEqual(words[4:], ['-s', '4', '-n', 'realesr-animevideov3',
                                 '-f', 'png', '-m', 'models'])
    w, h, rgba = self.images[_ICON]
    self.assertEqual(self._pack_image(_ICON),
                     (w * 4, h * 4, texture_pack.nearest(w, h, rgba, 4, 4)))

  def test_command_line(self):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
      code = texture_pack.main(['upscale', self.dump, self.pack, '--scale',
                                '2', '--top', '2', '--'] + self._command())
    self.assertEqual(code, 0, out.getvalue())
    self.assertIn('2 written', out.getvalue())
    with contextlib.redirect_stderr(io.StringIO()) as err:
      code = texture_pack.main(['upscale', self.dump, self.pack,
                                '--realesrgan',
                                os.path.join(self.root, 'missing-scaler')])
    self.assertEqual(code, 1)
    self.assertIn('not found', err.getvalue())


class CheckTest(_DumpCase):

  def setUp(self):
    super().setUp()
    self.pack = os.path.join(self.root, 'pack')
    os.makedirs(self.pack)
    with open(os.path.join(self.pack, 'textures.ini'), 'w') as out:
      out.write(texture_pack.DEFAULT_TEXTURES_INI)

  def _put(self, name, width, height, alpha=None):
    texture_pack.save_png(os.path.join(self.pack, name), width, height,
                          _gradient(width, height, alpha))

  def _ini(self, text):
    with open(os.path.join(self.pack, 'textures.ini'), 'w') as out:
      out.write(text)

  def test_a_good_pack(self):
    self._put(f'{_WALL}.png', 64, 32)
    self._put(f'{_ICON}.png', 8, 4)  # x1 is fine.
    self._put(f'{_OLD}.png', 8, 8)  # Its size comes from the dump's PNG.
    os.makedirs(os.path.join(self.pack, 'hd'))
    self._put(os.path.join('hd', 'leaf.png'), 16, 16)
    self._ini(texture_pack.DEFAULT_TEXTURES_INI + f'{_LEAF} = hd/leaf.png\n')
    result = texture_pack.check(self.pack, self.dump)
    self.assertEqual(result.errors, [])
    self.assertEqual(result.warnings, [])
    self.assertEqual(result.checked, 4)

  def test_errors(self):
    self._put('ffffffffffffffffffffffff.png', 8, 8)  # Not in the dump.
    self._put(f'{_WALL}.png', 24, 12)  # x1.5.
    self._put(f'{_LEAF}.png', 8, 16)  # Another aspect.
    # Larger than 4096 (1-bit grey, so the test stays small).
    rows = (b'\x00' + bytes(1024)) * 4096
    with open(os.path.join(self.pack, f'{_ICON}.png'), 'wb') as out:
      out.write(_raw_png(8192, 4096, 1, 0, rows))
    with open(os.path.join(self.pack, f'{_OLD}.png'), 'wb') as out:
      out.write(texture_pack.encode_png(2, 2, _gradient(2, 2))[:-8])
    result = texture_pack.check(self.pack, self.dump)
    text = '\n'.join(result.errors)
    self.assertEqual(len(result.errors), 5, text)
    self.assertIn('ffffffffffffffffffffffff is not in the dump', text)
    self.assertIn('24x12 is not a whole multiple of the original 16x8', text)
    self.assertIn('8x16 is not a whole multiple of the original 4x4', text)
    self.assertIn('larger than 4096', text)
    self.assertIn(f'{_OLD}.png: truncated', text)

  def test_names_and_ini(self):
    self._put('readme.png', 4, 4)
    self._put(f'{_WALL.upper()[:-8]}{_WALL[-8:]}.png', 32, 16)
    self._put(f'{_WALL}_1.png', 8, 4)
    with open(os.path.join(self.pack, 'dump.csv'), 'w') as out:
      out.write(texture_pack.DUMP_HEADER + '\n')
    self._ini(texture_pack.DEFAULT_TEXTURES_INI +
              f'{_ICON} = ../outside.png\n'
              f'{_LEAF} = missing.png\n'
              f'{_GHOST[:16]} = readme.png\n'
              '[hashranges]\n')
    result = texture_pack.check(self.pack, self.dump)
    errors = '\n'.join(result.errors)
    warnings = '\n'.join(result.warnings)
    self.assertIn('../outside.png: the file must be a relative path', errors)
    self.assertIn('missing.png does not exist', errors)
    self.assertIn('00000000000a010400000000 is not in the dump', errors)
    self.assertIn('[hashranges]', warnings)
    self.assertIn('reads as 00000000000a010400000000', warnings)
    self.assertIn(f'write it as {_WALL}.png', warnings)
    self.assertIn('mip level 1 is not checked', warnings)
    self.assertIn('dump.csv: a pack does not need', warnings)
    self.assertNotIn('readme.png: not named by a key', warnings)

  def test_ini_problems(self):
    os.remove(os.path.join(self.pack, 'textures.ini'))
    self.assertIn('no textures.ini',
                  texture_pack.check(self.pack, self.dump).errors[0])
    self._ini('[options]\nhash = quick\n')
    self.assertIn('hash = quick is not supported',
                  texture_pack.check(self.pack, self.dump).errors[0])
    self._ini('[options]\nhash = xxh64\nignoreAddress = false\n')
    self.assertIn('the keys would not match',
                  texture_pack.check(self.pack, self.dump).errors[0])
    self._ini('[options]\nhash = xxh64\nignoreAddress = true\n')
    self._put('notes.png', 4, 4)
    result = texture_pack.check(self.pack, self.dump)
    self.assertEqual(result.errors, [])
    self.assertIn('notes.png: not named by a key', result.warnings[0])

  def test_ini_parser_like_the_runtime(self):
    ini = texture_pack.parse_textures_ini(
        '﻿[Options]\nHash = XXH64 # comment\nignoreAddress = 1\n'
        '[hashes]\n'
        f'{_WALL} = "a#b.png"\n'
        f'{_ICON} = c\\#d.png|e.png # mip 1\n')
    self.assertEqual((ini.hash, ini.ignore_address), ('xxh64', True))
    self.assertEqual([h[3] for h in ini.hashes], ['a#b.png', 'c#d.png'])
    self.assertEqual(texture_pack.parse_hash_name(' 1234'),
                     ('000000000000123400000000', 0))
    self.assertEqual(texture_pack.parse_hash_name(f'{_WALL}_2'), (_WALL, 2))
    with self.assertRaises(texture_pack.PackError):
      texture_pack.parse_textures_ini('[options]\nreduceHash = true\n'
                                      'hash = xxh64\n')
    with self.assertRaises(texture_pack.PackError):
      texture_pack.parse_textures_ini('[options]\nversion = 1\n')

  def test_command_line(self):
    self._put(f'{_WALL}.png', 24, 12)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
      self.assertEqual(texture_pack.main(['check', self.pack, self.dump]), 1)
    self.assertIn('1 errors', out.getvalue())
    os.remove(os.path.join(self.pack, f'{_WALL}.png'))
    with contextlib.redirect_stdout(io.StringIO()):
      self.assertEqual(texture_pack.main(['check', self.pack, self.dump]), 0)


if __name__ == '__main__':
  unittest.main()
