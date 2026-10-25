#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Imports a decrypted PSP module into the local Ghidra project, relocated at
# the given base, runs the auto-analysis, seeds the functions Ghidra misses
# (relocations, then prologues, then gaps; see MpoSeedFunctions.java), names
# the imports and exports the analysis (JSON) for docs/analysis/ and the
# recompiler. The metrics are also taken after each seeding step
# (out/<program>/1-ghidra, 2-relocations, 3-prologues; the final ones are in
# out/<program>/). Everything it writes goes under $MPO_ANALYSIS_DIR (ignored by
# git): the project in ghidra/, a copy of the module in input/ and the outputs
# in out/<program>/.
#
# Usage:
#   profiles/mpo/tools/ghidra/run_analysis.sh <module.elf> <program> <image_base>
#   profiles/mpo/tools/ghidra/run_analysis.sh game/elf/mgp_main.elf mgp_main 08804000
#
# Environment:
#   GHIDRA_INSTALL_DIR  Ghidra 12.1.3 with ghidra-allegrex v21.4 (required)
#   JAVA_HOME           JDK 21 (required unless java 21 is on PATH)
#   MPO_ANALYSIS_DIR    default: <repo>/analysis
#   MPO_NID_TABLE       default: $MPO_ANALYSIS_DIR/nids/ppsspp-v1.20.4.tsv
#                       (profiles/mpo/tools/build_nid_table.py --fetch ...)
#   MPO_ANCHORS         default: profiles/mpo/config/anchors.txt
#   GHIDRA_HEADLESS_MAXMEM  default: 4G
#   MPO_EXTERNAL_BLOCK  optional "<name> <hex start> <hex size>": a stand-in
#                       block (jr ra; nop filler, not executable) for a module
#                       the program calls into (for a stage
#                       overlay: "mgp_main 08804000 2F7800"; see
#                       MpoExternalBlock.java and docs/formats/rlc.md)
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <module.elf> <program> <image_base>" >&2
  exit 2
fi
module=$1
program=$2
base=$3

script_dir=$(cd "$(dirname "$0")" && pwd)
repo_dir=$(cd "$script_dir/../../../.." && pwd)
: "${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to the Ghidra 12.1.3 install}"
analysis_dir=${MPO_ANALYSIS_DIR:-$repo_dir/analysis}
nid_table=${MPO_NID_TABLE:-$analysis_dir/nids/ppsspp-v1.20.4.tsv}
anchors=${MPO_ANCHORS:-$repo_dir/profiles/mpo/config/anchors.txt}
export GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-4G}

# Optional pre-script arguments (word splitting is intended).
pre_script=
if [ -n "${MPO_EXTERNAL_BLOCK:-}" ]; then
  pre_script="-preScript MpoExternalBlock.java $MPO_EXTERNAL_BLOCK"
fi

case "$program" in
  *[!A-Za-z0-9_]* | '') echo "error: program name must be [A-Za-z0-9_]+" >&2; exit 2 ;;
esac
if [ ! -f "$nid_table" ]; then
  echo "error: NID table $nid_table not found; build it with" >&2
  echo "  python -I profiles/mpo/tools/build_nid_table.py --fetch" \
       "$analysis_dir/ppsspp-v1.20.4/Core/HLE $nid_table" >&2
  exit 2
fi

mkdir -p "$analysis_dir/ghidra" "$analysis_dir/input" "$analysis_dir/out/$program"
# Ghidra names the program after the file (and resolves symlinks), so import a
# copy named exactly <program>.
cp "$module" "$analysis_dir/input/$program"
out=$analysis_dir/out/$program

"$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "$analysis_dir/ghidra" mpo \
  -import "$analysis_dir/input/$program" -overwrite \
  -loader PspElfLoader -loader-imagebase "$base" \
  -analysisTimeoutPerFile 3600 \
  -scriptPath "$script_dir" \
  $pre_script \
  -postScript MpoMetrics.java "$out/1-ghidra" \
  -postScript MpoSeedFunctions.java "$out" relocations \
  -postScript MpoMetrics.java "$out/2-relocations" \
  -postScript MpoSeedFunctions.java "$out" prologues \
  -postScript MpoMetrics.java "$out/3-prologues" \
  -postScript MpoSeedFunctions.java "$out" gaps \
  -postScript MpoModuleTables.java "$nid_table" "$out" \
  -postScript MpoMetrics.java "$out" \
  -postScript MpoExportRecompInput.java "$out" \
  -postScript MpoFindAnchors.java "$anchors" "$out" \
  > "$out/headless.log" 2>&1 || {
    echo "error: analyzeHeadless failed, see $out/headless.log" >&2
    exit 1
  }
# Script exceptions do not change the exit code of analyzeHeadless.
if grep -qE '^ERROR|[A-Za-z]Exception(:|$)' "$out/headless.log"; then
  echo "error: a script failed, see $out/headless.log" >&2
  exit 1
fi
grep -E '(EXTERNAL-BLOCK|SEED|MODULES|METRICS|RECOMP-INPUT|ANCHORS) ' "$out/headless.log" | sed 's/^INFO  //; s/ (GhidraScript)//'
echo "outputs in $out"
