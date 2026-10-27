#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the recompiled C++ of a module (F07b, F08b): installed -> analysis -> generated.

The module is mgp_main (the default), one of the resident Konami modules of
profiles/mpo/config/modules.toml (sound2, kjfs, zlibdec; F08b) or a stage
overlay (--stage <s>, F13).

Pipeline, all from the user's own files (nothing here is versioned):

  1. Checks the input against its SHA-256 in profiles/mpo/config/supported.toml:
     the installed executable (`mpo --install`, F02) for mgp_main, the PRX
     extracted to <game-dir>/disc/ for a module. A stage is
     stage/<s>/<s>.prx plus its stage/<s>/_zar, linked with the <s>.rlc inside
     the _zar against mgp_main (rlc.py link, F04) into <out>/stage_<s>.prx.
  2. recomp_hints.py: the Ghidra export (F03, run_analysis.sh) plus the
     hand-written hints in profiles/mpo/config/hints/<module>.txt, if any ->
     <out>/<module>.hints.txt.
  3. psp_recomp <elf> --auto <out>/generated <base> 0x4000
         --hints <out>/<module>.hints.txt --vfpu-executor [--prefix <module>]
     mgp_main is generated for 0x08804000 with no prefix; a module for its
     `base` in modules.toml and a stage for its base in stages.toml, with
     their name (stage_<s> for a stage) as the symbol prefix (so the dense
     unit table of mgp_main stays its own).
  4. Writes <out>/generate.json: the command, the SHA-256 of every input
     (executable, Ghidra export, hints) and of every generated file, so a
     second run can be checked byte for byte (--check); psp_recomp's own hash
     is recorded too, but it depends on the compiler that built it. A stage
     also gets <out>/overlay.txt, the identity of its overlay library: the
     SHA-256 of <s>.prx, the base, the image size and the SHA-256 of its .text
     as the game leaves it after applying the .rlc (CMake reads it).
  5. Optionally (--coverage) runs mpo_analysis_export and recomp_compare.py to
     measure how much of Ghidra's code the analysis covers.

Usage (paths default to the checkout this script lives in):

  python -I profiles/mpo/scripts/generate.py --psp-recomp build/framework/psprecomp/psp_recomp \\
      [--module mgp_main|sound2|kjfs|zlibdec | --stage <s>] \\
      [--game-dir game] [--analysis-dir analysis] [--out analysis/f07b/mgp_main] \\
      [--analysis-export build/profiles/mpo/tools/mpo_analysis_export --coverage] [--check]

The output goes to `--out` (default: profiles/mpo/generated/<module>, ignored
by git). Build mgp_main with -DMPO_GENERATED_DIR=<out>/generated, the modules
with -DMPO_GENERATED_MODULES_DIR=<the directory holding <module>/generated>
and the stages with -DMPO_GENERATED_OVERLAYS_DIR=<the directory holding
<s>/overlay.txt> (build_overlays.py generates several stages at once).
"""

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tomllib

_ROOT = pathlib.Path(__file__).resolve().parents[3]
_PROFILE = _ROOT / 'profiles' / 'mpo'
_MAIN = 'mgp_main'
_MAIN_LOAD_BASE = '0x08804000'
_UNIT_SPAN = '0x4000'
_STAGE_DIR = 'PSP_GAME/USRDIR/stage'


class GenerateError(Exception):
  """Raised when an input is missing or does not match."""


def _sha256(path):
  digest = hashlib.sha256()
  with open(path, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
      digest.update(block)
  return digest.hexdigest()


def _load_tool(name):
  # Loaded by path: `python -I` drops the script directory from sys.path.
  path = _PROFILE / 'tools' / f'{name}.py'
  spec = importlib.util.spec_from_file_location(name, path)
  module = importlib.util.module_from_spec(spec)
  spec.loader.exec_module(module)
  return module


def _supported_files(version):
  return {entry.get('path'): entry['sha256'] for entry in version.get('file', [])}


def _main_elf(game_dir, version):
  for module in version.get('decrypt', []):
    if module.get('module') == _MAIN:
      return (game_dir / 'elf' / module['output'], f'game/elf/{module["output"]}', module['sha256'])
  raise GenerateError(f'no {_MAIN} in the decrypt list of {version["disc_id"]}')


def _check_sha256(path, expected, what):
  if not path.is_file():
    raise GenerateError(f'missing {what}: {path} (run `mpo --install`)')
  actual = _sha256(path)
  if actual != expected:
    raise GenerateError(f'{path}: SHA-256 {actual} is not the supported {what} ({expected})')


def _target(name, game_dir, supported_path, modules_path):
  """Returns the input of `name`: (elf path, label, sha256, base, prefix, disc)."""
  with open(supported_path, 'rb') as f:
    supported = tomllib.load(f)
  for version in supported.get('version', []):
    disc = f'{version["disc_id"]} {version["disc_version"]}'
    if name == _MAIN:
      for module in version.get('decrypt', []):
        if module.get('module') == _MAIN:
          return (game_dir / 'elf' / module['output'], f'game/elf/{module["output"]}', module['sha256'],
                  _MAIN_LOAD_BASE, None, disc)
      continue
    with open(modules_path, 'rb') as f:
      modules = tomllib.load(f)
    for module in modules.get('module', []):
      if module.get('name') != name:
        continue
      for entry in version.get('file', []):
        if entry.get('path') == module['file']:
          return (game_dir / 'disc' / module['file'], f'game/disc/{module["file"]}', entry['sha256'],
                  f'0x{module["base"]:08X}', name, disc)
      raise GenerateError(f'{supported_path}: no file entry for {module["file"]}')
  raise GenerateError(f'unknown module {name!r} (not mgp_main, not in {modules_path})')


def stage_base(stage, stages_path):
  """Returns the base stage `stage` is recompiled for (stages.toml)."""
  with open(stages_path, 'rb') as f:
    stages = tomllib.load(f)
  for entry in stages.get('stage', []):
    if entry.get('name') == stage:
      return entry['base']
  return stages['default_base']


def link_stage(stage, game_dir, supported_path, stages_path, out):
  """Checks and links stage `stage`; returns its target and overlay identity.

  Writes <out>/stage_<stage>.prx (the PRX with the .rlc applied: game bytes,
  never versioned). Returns ((elf, label, sha256, base, prefix, disc), inputs,
  overlay) where `inputs` maps the checked files to their SHA-256 and
  `overlay` is the dictionary written to overlay.txt.
  """
  if not stage or not all(c.isalnum() or c == '_' for c in stage):
    raise GenerateError(f'invalid stage name {stage!r}')
  with open(supported_path, 'rb') as f:
    supported = tomllib.load(f)
  psp_prx = _load_tool('psp_prx')
  rlc = _load_tool('rlc')
  zar = _load_tool('zar')
  for version in supported.get('version', []):
    files = _supported_files(version)
    prx_file = f'{_STAGE_DIR}/{stage}/{stage}.prx'
    zar_file = f'{_STAGE_DIR}/{stage}/_zar'
    if prx_file not in files:
      continue
    if zar_file not in files:
      raise GenerateError(f'{supported_path}: no file entry for {zar_file}')
    main_path, main_label, main_sha256 = _main_elf(game_dir, version)
    prx_path = game_dir / 'disc' / prx_file
    zar_path = game_dir / 'disc' / zar_file
    _check_sha256(main_path, main_sha256, _MAIN)
    _check_sha256(prx_path, files[prx_file], f'{stage}.prx')
    _check_sha256(zar_path, files[zar_file], f'{stage} _zar')
    base = stage_base(stage, stages_path)
    try:
      prx = psp_prx.Prx(prx_path.read_bytes())
      main = psp_prx.Prx(main_path.read_bytes())
      dar = zar.decompress(zar_path.read_bytes())
      table = zar.read_entry(dar, zar.parse_dar(dar), f'{stage}.rlc')
      main_base = int(_MAIN_LOAD_BASE, 16)
      linked = rlc.link(prx, table, main, main_base)
      image = rlc.relocate(prx, table, main, base, main_base)
    except (psp_prx.PrxError, rlc.RlcError, zar.ZarError) as error:
      raise GenerateError(f'{stage}: {error}') from error
    text = prx.section('.text')
    if text is None or text.size == 0 or text.addr + text.size > len(image):
      raise GenerateError(f'{stage}: no .text section in the PRX')
    out.mkdir(parents=True, exist_ok=True)
    name = f'stage_{stage}'
    elf = out / f'{name}.prx'
    elf.write_bytes(linked)
    overlay = {
        'name': stage,
        'prefix': name,
        'base': f'0x{base:08X}',
        'size': f'0x{len(image):X}',
        'prx_sha256': files[prx_file],
        'text_offset': f'0x{text.addr:X}',
        'text_size': f'0x{text.size:X}',
        'text_sha256': hashlib.sha256(image[text.addr:text.addr + text.size]).hexdigest(),
    }
    inputs = {
        main_label: main_sha256,
        f'game/disc/{prx_file}': files[prx_file],
        f'game/disc/{zar_file}': files[zar_file],
    }
    disc = f'{version["disc_id"]} {version["disc_version"]}'
    return ((elf, f'<out>/{elf.name}', _sha256(elf), f'0x{base:08X}', name, disc), inputs,
            overlay)
  raise GenerateError(f'unknown stage {stage!r} (no {_STAGE_DIR}/{stage}/{stage}.prx in '
                      f'{supported_path})')


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

  overlay = None
  if args.stage:
    target, inputs, overlay = link_stage(args.stage, game_dir, supported,
                                         _PROFILE / 'config' / 'stages.toml', out)
    elf, elf_label, elf_sha256, load_base, prefix, disc = target
    name = prefix
  else:
    name = args.module
    elf, elf_label, elf_sha256, load_base, prefix, disc = _target(
        name, game_dir, supported, _PROFILE / 'config' / 'modules.toml')
    inputs = {elf_label: elf_sha256}
  extra_hints = _PROFILE / 'config' / 'hints' / f'{name}.txt'
  recomp_json = analysis_dir / 'out' / name / f'{name}.recomp.json'
  ghidra_hint = 'run profiles/mpo/tools/ghidra/run_analysis.sh'
  if args.stage:
    ghidra_hint = (f'MPO_EXTERNAL_BLOCK="mgp_main 08804000 2F7800" '
                   f'profiles/mpo/tools/ghidra/run_analysis.sh {elf} {name} {load_base[2:]}')
  for path, what in ((elf, 'installed input (run `mpo --install`)'),
                     (recomp_json, f'Ghidra export ({ghidra_hint})'),
                     (psp_recomp, 'psp_recomp (build the framework)')):
    if not path.is_file():
      raise GenerateError(f'missing {what}: {path}')
  actual = _sha256(elf)
  if actual != elf_sha256:
    raise GenerateError(f'{elf}: SHA-256 {actual} is not the supported {name} ({elf_sha256})')

  out.mkdir(parents=True, exist_ok=True)
  generated = out / 'generated'
  hints = out / f'{name}.hints.txt'
  prefix_args = ['--prefix', prefix] if prefix else []
  with open(out / 'generate.log', 'w', encoding='utf-8') as log:
    extra = ['--extra', extra_hints] if extra_hints.is_file() else []
    _run([sys.executable, '-I', _PROFILE / 'tools' / 'recomp_hints.py', recomp_json, *extra,
          '--output', hints], log)
    command = [psp_recomp, elf, '--auto', generated, load_base, _UNIT_SPAN,
               '--hints', hints, '--vfpu-executor', *prefix_args]
    summary = _run(command, log)
    coverage = None
    if args.coverage:
      if not args.analysis_export:
        raise GenerateError('--coverage needs --analysis-export')
      export = out / f'{name}.export.json'
      _run([args.analysis_export, elf, load_base, export, hints], log)
      coverage_out = _run([sys.executable, '-I', _PROFILE / 'tools' / 'recomp_compare.py',
                           '--export', export, '--ghidra', recomp_json.parent], log)
      coverage = coverage_out.splitlines()

  report = json.loads((generated / 'auto_codegen_report.json').read_text(encoding='utf-8'))
  inputs.update({
      f'analysis/out/{name}/{recomp_json.name}': _sha256(recomp_json),
      hints.name: _sha256(hints),
  })
  if extra_hints.is_file():
    inputs[f'profiles/mpo/config/hints/{extra_hints.name}'] = _sha256(extra_hints)
  if overlay is not None:
    (out / 'overlay.txt').write_text(
        ''.join(f'{key}={value}\n' for key, value in overlay.items()), encoding='utf-8')
  return {
      'module': name,
      'disc': disc,
      'command': ['psp_recomp', elf_label, '--auto', '<out>/generated', load_base,
                  _UNIT_SPAN, '--hints', f'<out>/{hints.name}', '--vfpu-executor', *prefix_args],
      'inputs': inputs,
      # Depends on the compiler that built it: informative, not checked.
      'tools': {'psp_recomp': _sha256(psp_recomp)},
      'report': report,
      'psp_recomp_summary': summary.splitlines(),
      'coverage': coverage,
      'overlay': overlay,
      'generated': _generated_hashes(generated),
  }


def parse_args(argv):
  parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
  parser.add_argument('--psp-recomp', required=True, help='psp_recomp from this checkout\'s framework')
  which = parser.add_mutually_exclusive_group()
  which.add_argument('--module', default=_MAIN,
                     help='mgp_main (default) or a module of profiles/mpo/config/modules.toml')
  which.add_argument('--stage', help='a stage overlay: stage/<s>/<s>.prx with its .rlc')
  parser.add_argument('--game-dir', default=str(_ROOT / 'game'))
  parser.add_argument('--analysis-dir', default=str(_ROOT / 'analysis'))
  parser.add_argument('--out', help='output directory (default: profiles/mpo/generated/<module>)')
  parser.add_argument('--analysis-export', help='mpo_analysis_export, for --coverage')
  parser.add_argument('--coverage', action='store_true', help='measure the coverage of Ghidra\'s code')
  parser.add_argument('--check', action='store_true',
                      help='fail unless the generated files match the previous generate.json')
  args = parser.parse_args(argv)
  if args.out is None:
    default = f'overlays/{args.stage}' if args.stage else args.module
    args.out = str(_PROFILE / 'generated' / default)
  return args


def main(argv):
  args = parse_args(argv)
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
