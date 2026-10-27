#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the recompiled stage overlays (F13), one corpus per distinct stage.

For each stage it runs generate.py --stage <s> (check against supported.toml,
link the .rlc, Ghidra hints, psp_recomp --prefix stage_<s>) into
<out>/<s>/. Each <out>/<s>/ then becomes one overlay library when the build is
configured with -DMPO_GENERATED_OVERLAYS_DIR=<out>; mpo_boot loads them
(--overlays) and installs the one whose <s>.prx SHA-256 and base match the
stage the game loads.

Stages that share a PRX (r_main01 = r_net01, st08 = st0801 = st0802...) also
share their .rlc (F13 [V]) and so their corpus: only the first one of each
(PRX, base) pair is generated.

With --ghidra, a stage without a Ghidra export is analysed first
(run_analysis.sh on the linked PRX, with mgp_main as the external block); that
needs GHIDRA_INSTALL_DIR (and JAVA_HOME) like run_analysis.sh itself.

Usage:

  python -I profiles/mpo/scripts/build_overlays.py --psp-recomp build/framework/psprecomp/psp_recomp \\
      (--stages init,r_main01,title | --all) [--out profiles/mpo/generated/overlays] \\
      [--ghidra] [--game-dir game] [--analysis-dir analysis]
"""

import argparse
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import time
import tomllib

_ROOT = pathlib.Path(__file__).resolve().parents[3]
_PROFILE = _ROOT / 'profiles' / 'mpo'
_MAIN_BLOCK = 'mgp_main 08804000 2F7800'


def _load_generate():
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = pathlib.Path(__file__).resolve().parent / 'generate.py'
  spec = importlib.util.spec_from_file_location('generate', path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def all_stages(supported_path):
  """Returns every stage name of supported.toml, sorted."""
  with open(supported_path, 'rb') as f:
    supported = tomllib.load(f)
  names = set()
  for version in supported.get('version', []):
    for entry in version.get('file', []):
      parts = entry.get('path', '').split('/')
      if (len(parts) == 5 and parts[:3] == ['PSP_GAME', 'USRDIR', 'stage'] and
          parts[4] == f'{parts[3]}.prx'):
        names.add(parts[3])
  return sorted(names)


def distinct_stages(stages, supported_path, stages_path, generate):
  """Keeps the first stage of each (PRX SHA-256, base); returns (kept, aliases)."""
  with open(supported_path, 'rb') as f:
    supported = tomllib.load(f)
  hashes = {}
  for version in supported.get('version', []):
    for entry in version.get('file', []):
      hashes[entry.get('path')] = entry['sha256']
  kept, aliases, seen = [], {}, {}
  for stage in stages:
    prx = hashes.get(f'PSP_GAME/USRDIR/stage/{stage}/{stage}.prx')
    if prx is None:
      raise generate.GenerateError(f'unknown stage {stage!r}')
    key = (prx, generate.stage_base(stage, stages_path))
    if key in seen:
      aliases[stage] = seen[key]
      continue
    seen[key] = stage
    kept.append(stage)
  return kept, aliases


def _run_ghidra(generate, stage, args):
  out = pathlib.Path(args.out) / stage
  target, _, _ = generate.link_stage(stage, pathlib.Path(args.game_dir),
                                     _PROFILE / 'config' / 'supported.toml',
                                     _PROFILE / 'config' / 'stages.toml', out)
  elf, _, _, base, name, _ = target
  if 'GHIDRA_INSTALL_DIR' not in os.environ:
    raise generate.GenerateError('--ghidra needs GHIDRA_INSTALL_DIR (see run_analysis.sh)')
  env = dict(os.environ, MPO_EXTERNAL_BLOCK=_MAIN_BLOCK,
             MPO_ANALYSIS_DIR=str(pathlib.Path(args.analysis_dir).resolve()))
  log = out / 'ghidra.log'
  print(f'build_overlays: {stage}: Ghidra ({log})', flush=True)
  with open(log, 'w', encoding='utf-8') as log_file:
    result = subprocess.run(
        ['sh', str(_PROFILE / 'tools' / 'ghidra' / 'run_analysis.sh'), str(elf), name, base[2:]],
        stdout=log_file, stderr=subprocess.STDOUT, env=env, check=False)
  if result.returncode != 0:
    raise generate.GenerateError(f'{stage}: run_analysis.sh failed, see {log}')


def build(args, generate):
  """Generates every requested stage; returns the summary rows."""
  supported = _PROFILE / 'config' / 'supported.toml'
  stages = all_stages(supported) if args.all else [s for s in args.stages.split(',') if s]
  kept, aliases = distinct_stages(stages, supported, _PROFILE / 'config' / 'stages.toml', generate)
  for alias, stage in sorted(aliases.items()):
    print(f'build_overlays: {alias} uses the corpus of {stage} (same PRX and base)')
  rows = []
  for stage in kept:
    out = pathlib.Path(args.out) / stage
    recomp_json = pathlib.Path(args.analysis_dir) / 'out' / f'stage_{stage}' / f'stage_{stage}.recomp.json'
    if args.ghidra and not recomp_json.is_file():
      _run_ghidra(generate, stage, args)
    start = time.monotonic()
    argv = ['--psp-recomp', args.psp_recomp, '--stage', stage, '--game-dir', args.game_dir,
            '--analysis-dir', args.analysis_dir, '--out', str(out)]
    if generate.main(argv) != 0:
      raise generate.GenerateError(f'{stage}: generate.py failed')
    manifest = json.loads((out / 'generate.json').read_text(encoding='utf-8'))
    size = sum(p.stat().st_size for p in (out / 'generated').glob('*.cpp'))
    rows.append((stage, manifest['overlay']['base'], manifest['report']['translation_units'],
                 size, time.monotonic() - start))
  return rows


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--psp-recomp', required=True, help='psp_recomp from this checkout\'s framework')
  which = parser.add_mutually_exclusive_group(required=True)
  which.add_argument('--stages', help='comma-separated stage names (stage/<s>/<s>.prx)')
  which.add_argument('--all', action='store_true', help='every stage of supported.toml')
  parser.add_argument('--out', default=str(_PROFILE / 'generated' / 'overlays'))
  parser.add_argument('--ghidra', action='store_true', help='analyse stages without a Ghidra export')
  parser.add_argument('--game-dir', default=str(_ROOT / 'game'))
  parser.add_argument('--analysis-dir', default=str(_ROOT / 'analysis'))
  args = parser.parse_args(argv)
  generate = _load_generate()
  try:
    rows = build(args, generate)
  except (OSError, ValueError, KeyError, generate.GenerateError) as error:
    print(f'build_overlays: {error}', file=sys.stderr)
    return 1
  print(f'\n{"stage":<16} {"base":<10} {"units":>5} {"C++ MB":>7} {"seconds":>8}')
  for stage, base, units, size, seconds in rows:
    print(f'{stage:<16} {base:<10} {units:>5} {size / 1e6:>7.1f} {seconds:>8.1f}')
  print(f'\nbuild_overlays: {len(rows)} overlays in {args.out}; configure with '
        f'-DMPO_GENERATED_OVERLAYS_DIR={args.out}')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
