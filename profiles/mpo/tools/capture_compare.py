#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares two folders of mpo_boot captures (--capture) frame by frame.

F15b: the game running with --renderer soft (the reference) and with
--renderer vulkan, with the same input, writes the same flips to two folders;
this pairs the PNGs by file name and applies mpo_vulkan_compare's threshold
(F15a): a frame fails when more than --max-pixels pixels differ or any
channel differs by more than --max-delta.

  python3 -I profiles/mpo/tools/capture_compare.py soft_dir vulkan_dir \
      [--max-pixels 130] [--max-delta 24] [--alpha] [--diff-dir DIR] \
      [--pattern flip-] [--quiet]

- Only the standard library is used (the PNG reader of ge_frame_diff.py).
- Identical files are not decoded, so long runs that match cost little.
- A name present in only one folder is a failure: the runs did not flip the
  same frames (or one stopped early).
- --diff-dir writes <name>.diff.png for each frame that differs: game pixels,
  so keep it in an ignored folder such as analysis/f15b/.

Exit status: 0 when every frame is within the threshold, 1 when not, 2 on
errors.
"""

import argparse
import importlib.util
import os
import pathlib
import sys


def _load_sibling(name):
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


ge_frame_diff = _load_sibling('ge_frame_diff')


def _list_pngs(folder, prefix):
  return sorted(name for name in os.listdir(folder)
                if name.endswith('.png') and name.startswith(prefix))


def compare_folders(reference_dir, other_dir, max_pixels=130, max_delta=24,
                    alpha=False, diff_dir=None, prefix='', report=print):
  """Compares the PNGs of two folders; returns a summary dict.

  `report` receives one line per frame that differs (and per missing frame).
  """
  ref_names = _list_pngs(reference_dir, prefix)
  other_names = _list_pngs(other_dir, prefix)
  ref_set = set(ref_names)
  other_set = set(other_names)
  summary = {
      'frames': 0,
      'identical': 0,
      'within': 0,
      'failed': 0,
      'missing': sorted(ref_set ^ other_set),
      'worst_pixels': 0,
      'worst_delta': 0,
      'failures': [],
  }
  for name in summary['missing']:
    where = reference_dir if name in ref_set else other_dir
    report(f'{name}: only in {where}')
  for name in ref_names:
    if name not in other_set:
      continue
    summary['frames'] += 1
    with open(os.path.join(reference_dir, name), 'rb') as f:
      ref_bytes = f.read()
    with open(os.path.join(other_dir, name), 'rb') as f:
      other_bytes = f.read()
    if ref_bytes == other_bytes:
      summary['identical'] += 1
      continue
    ref = ge_frame_diff.read_png(ref_bytes)
    other = ge_frame_diff.read_png(other_bytes)
    if ref[0] != other[0] or ref[1] != other[1]:
      raise ge_frame_diff.PngError(
          f'{name}: sizes differ ({ref[0]}x{ref[1]} and {other[0]}x{other[1]})')
    result = ge_frame_diff.compare(other, ref, ref[0], ref[1], alpha)
    worst = max(result['max_delta']) if result['differing'] else 0
    summary['worst_pixels'] = max(summary['worst_pixels'], result['differing'])
    summary['worst_delta'] = max(summary['worst_delta'], worst)
    failed = result['differing'] > max_pixels or worst > max_delta
    if result['differing'] == 0:
      # Same pixels, different bytes (e.g. alpha when comparing RGB).
      summary['identical'] += 1
      continue
    if failed:
      summary['failed'] += 1
      summary['failures'].append(name)
    else:
      summary['within'] += 1
    box = result['box']
    report(f'{name}: {result["differing"]} pixels, max delta {worst}, '
           f'box ({box[0]},{box[1]})-({box[2]},{box[3]})'
           f'{" FAIL" if failed else ""}')
    if diff_dir:
      os.makedirs(diff_dir, exist_ok=True)
      stem = name[:-len('.png')]
      ge_frame_diff.write_png(os.path.join(diff_dir, stem + '.diff.png'),
                              ref[0], ref[1], result['diff'])
  return summary


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('reference', help='captures of --renderer soft')
  parser.add_argument('other', help='captures of --renderer vulkan')
  parser.add_argument('--max-pixels', type=int, default=130)
  parser.add_argument('--max-delta', type=int, default=24)
  parser.add_argument('--alpha', action='store_true',
                      help='also compare alpha (stencil)')
  parser.add_argument('--diff-dir', help='write <name>.diff.png here')
  parser.add_argument('--pattern', default='',
                      help='only files whose name starts with this')
  parser.add_argument('--quiet', action='store_true',
                      help='only the summary')
  args = parser.parse_args(argv)
  report = (lambda line: None) if args.quiet else print
  try:
    summary = compare_folders(args.reference, args.other, args.max_pixels,
                              args.max_delta, args.alpha, args.diff_dir,
                              args.pattern, report)
  except (OSError, ge_frame_diff.PngError) as error:
    print(f'error: {error}', file=sys.stderr)
    return 2
  print(f'{summary["frames"]} frames compared: {summary["identical"]} '
        f'identical, {summary["within"]} within the threshold '
        f'({args.max_pixels} pixels, delta {args.max_delta}), '
        f'{summary["failed"]} failed, {len(summary["missing"])} missing; '
        f'worst {summary["worst_pixels"]} pixels, delta '
        f'{summary["worst_delta"]}')
  if summary['frames'] == 0:
    print('error: no frames in common', file=sys.stderr)
    return 2
  return 1 if summary['failed'] or summary['missing'] else 0


if __name__ == '__main__':
  sys.exit(main())
