#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the recompiled C++ of mgp_main (F07b): installed -> analysis -> generated.

Pipeline, all from the user's own files (nothing here is versioned):

  1. Checks the installed executable (`mpo --install`, F02) against the SHA-256
     in profiles/mpo/config/supported.toml.
  2. recomp_hints.py: the Ghidra export (F03, run_analysis.sh) plus the
     hand-written hints in profiles/mpo/config/hints/ -> <out>/mgp_main.hints.txt.
  3. psp_recomp <elf> --auto <out>/generated 0x08804000 0x4000
         --hints <out>/mgp_main.hints.txt --vfpu-executor
  4. Writes <out>/generate.json: the command, the SHA-256 of every input
     (executable, Ghidra export, hints) and of every generated file, so a
     second run can be checked byte for byte (--check); psp_recomp's own hash
     is recorded too, but it depends on the compiler that built it.
  5. Optionally (--coverage) runs mpo_analysis_export and recomp_compare.py to
     measure how much of Ghidra's code the analysis covers.

Usage (paths default to the checkout this script lives in):

  python -I profiles/mpo/scripts/generate.py --psp-recomp build/framework/psprecomp/psp_recomp \\
      [--game-dir game] [--analysis-dir analysis] [--out analysis/f07b/mgp_main] \\
      [--analysis-export build/profiles/mpo/tools/mpo_analysis_export --coverage] [--check]

The output goes to `--out` (default: profiles/mpo/generated/mgp_main, ignored
by git). Build it with -DMPO_GENERATED_DIR=<out>/generated.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tomllib

_ROOT = pathlib.Path(__file__).resolve().parents[3]
_PROFILE = _ROOT / 'profiles' / 'mpo'
_MODULE = 'mgp_main'
_LOAD_BASE = '0x08804000'
_UNIT_SPAN = '0x4000'


class GenerateError(Exception):
  """Raised when an input is missing or does not match."""


def _sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def _expected_elf(supported_path):
  with open(supported_path, 'rb') as f:
    supported = tomllib.load(f)
  for version in supported.get('version', []):
    for module in version.get('decrypt', []):
      if module.get('module') == _MODULE:
        return module['output'], module['sha256'], version['disc_id'], version['disc_version']
  raise GenerateError(f'{supported_path}: no decrypted {_MODULE} entry')


def _run(command, log):
  print('generate:', ' '.join(str(c) for c in command), flush=True)
  result = subprocess.run([str(c) for c in command], capture_output=True, text=True, check=False)
  log.write(result.stdout)
  log.write(result.stderr)
  if result.returncode != 0:
    raise GenerateError(f'{command[0]} failed with exit code {result.returncode}:\n{result.stderr[-2000:]}')
  return result.stdout


def _generated_hashes(directory):
  # auto_codegen_report.json counts the units rewritten by this very run, so
  # only the C++ is part of the reproducible output.
  return {p.name: _sha256(p) for p in sorted(directory.iterdir())
          if p.is_file() and p.suffix in ('.cpp', '.hpp')}


def generate(args):
  """Runs the pipeline; returns the manifest dictionary."""
  game_dir = pathlib.Path(args.game_dir)
  analysis_dir = pathlib.Path(args.analysis_dir)
  out = pathlib.Path(args.out)
  psp_recomp = pathlib.Path(args.psp_recomp)
  supported = _PROFILE / 'config' / 'supported.toml'
  extra_hints = _PROFILE / 'config' / 'hints' / f'{_MODULE}.txt'

  output_name, elf_sha256, disc_id, disc_version = _expected_elf(supported)
  elf = game_dir / 'elf' / output_name
  recomp_json = analysis_dir / 'out' / _MODULE / f'{_MODULE}.recomp.json'
  for path, what in ((elf, 'installed executable (run `mpo --install`)'),
                     (recomp_json, 'Ghidra export (run profiles/mpo/tools/ghidra/run_analysis.sh)'),
                     (psp_recomp, 'psp_recomp (build the framework)')):
    if not path.is_file():
      raise GenerateError(f'missing {what}: {path}')
  actual = _sha256(elf)
  if actual != elf_sha256:
    raise GenerateError(f'{elf}: SHA-256 {actual} is not the supported {_MODULE} ({elf_sha256})')

  out.mkdir(parents=True, exist_ok=True)
  generated = out / 'generated'
  hints = out / f'{_MODULE}.hints.txt'
  with open(out / 'generate.log', 'w', encoding='utf-8') as log:
    _run([sys.executable, '-I', _PROFILE / 'tools' / 'recomp_hints.py', recomp_json,
          '--extra', extra_hints, '--output', hints], log)
    command = [psp_recomp, elf, '--auto', generated, _LOAD_BASE, _UNIT_SPAN,
               '--hints', hints, '--vfpu-executor']
    summary = _run(command, log)
    coverage = None
    if args.coverage:
      if not args.analysis_export:
        raise GenerateError('--coverage needs --analysis-export')
      export = out / f'{_MODULE}.export.json'
      _run([args.analysis_export, elf, _LOAD_BASE, export, hints], log)
      coverage_out = _run([sys.executable, '-I', _PROFILE / 'tools' / 'recomp_compare.py',
                           '--export', export, '--ghidra', recomp_json.parent], log)
      coverage = coverage_out.splitlines()

  report = json.loads((generated / 'auto_codegen_report.json').read_text(encoding='utf-8'))
  return {
      'module': _MODULE,
      'disc': f'{disc_id} {disc_version}',
      'command': ['psp_recomp', f'game/elf/{output_name}', '--auto', '<out>/generated', _LOAD_BASE,
                  _UNIT_SPAN, '--hints', f'<out>/{hints.name}', '--vfpu-executor'],
      'inputs': {
          f'game/elf/{output_name}': elf_sha256,
          f'analysis/out/{_MODULE}/{recomp_json.name}': _sha256(recomp_json),
          f'profiles/mpo/config/hints/{extra_hints.name}': _sha256(extra_hints),
          hints.name: _sha256(hints),
      },
      # Depends on the compiler that built it: informative, not checked.
      'tools': {'psp_recomp': _sha256(psp_recomp)},
      'report': report,
      'psp_recomp_summary': summary.splitlines(),
      'coverage': coverage,
      'generated': _generated_hashes(generated),
  }


def main(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--psp-recomp', required=True, help='psp_recomp from this checkout\'s framework')
  parser.add_argument('--game-dir', default=str(_ROOT / 'game'))
  parser.add_argument('--analysis-dir', default=str(_ROOT / 'analysis'))
  parser.add_argument('--out', default=str(_PROFILE / 'generated' / _MODULE))
  parser.add_argument('--analysis-export', help='mpo_analysis_export, for --coverage')
  parser.add_argument('--coverage', action='store_true', help='measure the coverage of Ghidra\'s code')
  parser.add_argument('--check', action='store_true',
                      help='fail unless the generated files match the previous generate.json')
  args = parser.parse_args(argv)
  manifest_path = pathlib.Path(args.out) / 'generate.json'
  try:
    previous = None
    if args.check:
      if not manifest_path.is_file():
        raise GenerateError(f'--check: no previous {manifest_path}')
      previous = json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest = generate(args)
    if previous is not None:
      changed = sorted(name for name in set(previous['generated']) | set(manifest['generated'])
                       if previous['generated'].get(name) != manifest['generated'].get(name))
      if changed or previous['inputs'] != manifest['inputs']:
        raise GenerateError(f'not reproducible: inputs or {len(changed)} generated files differ '
                            f'({", ".join(changed[:5])})')
      print(f'generate: reproducible, {len(changed)} differences in {len(manifest["generated"])} files')
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
  except (OSError, ValueError, KeyError, GenerateError) as error:
    print(f'generate: {error}', file=sys.stderr)
    return 1
  report = manifest['report']
  print(f'generate: {report["translation_units"]} units, {report["unique_registered_block_entries"]} '
        f'entries, {report["import_wrappers"]} imports -> {manifest_path}')
  return 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
