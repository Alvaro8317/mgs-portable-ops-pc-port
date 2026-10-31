#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for profiles/mpo/tools/capture_compare.py with synthetic captures."""

import contextlib
import importlib.util
import io
import os
import pathlib
import tempfile
import unittest

_TOOLS = pathlib.Path(__file__).resolve().parent.parent / 'tools'


def _load_tool(name):
  # Loaded by path so the test also works under `python -I`.
  spec = importlib.util.spec_from_file_location(name, _TOOLS / f'{name}.py')
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


capture_compare = _load_tool('capture_compare')
ge_frame_diff = _load_tool('ge_frame_diff')

_W = 16
_H = 8


def _image(changes=()):
  """A gray RGBA image with (x, y, (r, g, b)) pixels changed."""
  pixels = bytearray(bytes((40, 40, 40, 255)) * (_W * _H))
  for x, y, rgb in changes:
    i = (y * _W + x) * 4
    pixels[i:i + 3] = bytes(rgb)
  return bytes(pixels)


def _write(folder, name, pixels):
  ge_frame_diff.write_png(os.path.join(folder, name), _W, _H, pixels)


class CaptureCompareTest(unittest.TestCase):

  def setUp(self):
    self._tmp = tempfile.TemporaryDirectory()
    self.soft = os.path.join(self._tmp.name, 'soft')
    self.vk = os.path.join(self._tmp.name, 'vk')
    os.makedirs(self.soft)
    os.makedirs(self.vk)

  def tearDown(self):
    self._tmp.cleanup()

  def _run(self, *extra):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
      status = capture_compare.main([self.soft, self.vk] + list(extra))
    return status, out.getvalue()

  def test_identical_frames_pass(self):
    for name in ('flip-000020-04000000.png', 'flip-000040-04044000.png'):
      _write(self.soft, name, _image())
      _write(self.vk, name, _image())
    status, out = self._run()
    self.assertEqual(status, 0)
    self.assertIn('2 frames compared: 2 identical', out)

  def test_threshold_on_pixels_and_delta(self):
    _write(self.soft, 'a.png', _image())
    _write(self.vk, 'a.png', _image([(1, 1, (50, 40, 40)), (2, 2, (40, 44, 40))]))
    status, out = self._run()
    self.assertEqual(status, 0)
    self.assertIn('1 within the threshold', out)
    self.assertIn('a.png: 2 pixels, max delta 10, box (1,1)-(2,2)', out)
    # One pixel too many, then a delta too large.
    status, out = self._run('--max-pixels', '1')
    self.assertEqual(status, 1)
    self.assertIn('FAIL', out)
    status, out = self._run('--max-delta', '9')
    self.assertEqual(status, 1)
    status, out = self._run('--max-pixels', '0', '--max-delta', '0')
    self.assertEqual(status, 1)

  def test_missing_frame_fails(self):
    _write(self.soft, 'a.png', _image())
    _write(self.vk, 'a.png', _image())
    _write(self.soft, 'b.png', _image())
    status, out = self._run()
    self.assertEqual(status, 1)
    self.assertIn('b.png: only in', out)
    self.assertIn('1 missing', out)

  def test_pattern_and_diff_images(self):
    _write(self.soft, 'flip-1.png', _image())
    _write(self.vk, 'flip-1.png', _image([(3, 4, (255, 0, 0))]))
    _write(self.soft, 'window-1.png', _image())
    diff_dir = os.path.join(self._tmp.name, 'diff')
    status, out = self._run('--pattern', 'flip-', '--diff-dir', diff_dir)
    self.assertEqual(status, 1)  # delta 215
    self.assertIn('1 frames compared', out)
    with open(os.path.join(diff_dir, 'flip-1.diff.png'), 'rb') as f:
      width, height, _ = ge_frame_diff.read_png(f.read())
    self.assertEqual((width, height), (_W, _H))

  def test_no_frames_is_an_error(self):
    status, out = self._run()
    self.assertEqual(status, 2)
    self.assertIn('no frames in common', out)


if __name__ == '__main__':
  unittest.main()
