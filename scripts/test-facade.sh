#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build=${LEANAT_CPP_BUILD:-"$root/build/linux-debug-on"}
systemc=${LEANAT_SYSTEMC_PREFIX:-"$root/.deps/systemc-linux-install"}
lake=${LEANAT_LAKE:-lake}
cxx=${CXX:-g++}
results=${LEANAT_FACADE_RESULTS:-"$root/build/facade-profiles"}
mkdir -p -- "$results"
scratch=$(mktemp -d "${TMPDIR:-/tmp}/leanat-facade.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
cd -- "$root"
# Standalone use must not combine new sources with stale Lean object files.
# Invalidate prior success even when the prerequisite build fails.
: > "$results/runs.json"
: > "$results/failed-runs.json"
"$lake" build LeanAT.Compiler.Main LeanAT.Frontend.BlockSyntax
manifest=(python3 "$root/scripts/facade-manifest.py")
"${manifest[@]}" begin "$root" "$results" "$scratch"
run_fixture() {
  local mode=$1 profile=$2 output driver
  local -a args
  output="$scratch/$profile-$mode"
  # Invalidate prior evidence before this run; a failed run cannot leave a previous success usable.
  : > "$results/$profile-$mode.json"
  : > "$results/$profile-$mode.execir.bin"
  args=("$output" "$profile")
  driver=facade_smoke
  if [[ $mode == wire ]]; then args+=(wire); driver=facade_wire; fi
  if [[ $mode == signal ]]; then args+=(signal); driver=facade_signal; fi
  if [[ $mode == process ]]; then args+=(process); driver=facade_process; fi
  "${manifest[@]}" step "$root" "$results" "$scratch" "$profile" "$mode" generate "$lake" env lean --run tests/lean/Facade.lean "${args[@]}" || return $?
  cp -- "$output/model.execir.bin" "$results/$profile-$mode.execir.bin" || return $?
  "${manifest[@]}" step "$root" "$results" "$scratch" "$profile" "$mode" compile "$cxx" -std=c++17 -DSC_ALLOW_DEPRECATED_IEEE_API -pthread \
    -I"$output/include" -I"$root/runtime/include" -I"$root/stdlib/include" \
    -I"$root/systemc/include" -I"$systemc/include" \
    "$root/tests/lean/$driver.cpp" "$output/src/Component_desc.cpp" \
    "$output/src/Component.cpp" "$output/src/Top.cpp" \
    "$build/systemc/libleanat_systemc.a" "$build/libleanat_stdlib.a" \
    "$build/libleanat_at.a" "$systemc/lib/libsystemc.a" \
    -o "$scratch/$profile-$driver" || return $?
  "${manifest[@]}" step "$root" "$results" "$scratch" "$profile" "$mode" run "$scratch/$profile-$driver" "$results/$profile-$mode.json" || return $?
}
failures=0
for profile in core ext; do
for mode in scalar wire signal process; do
  if run_fixture "$mode" "$profile"; then
    printf 'PASS generated facade %s %s\n' "$profile" "$mode"
  else
    status=$?
    printf 'FAIL generated facade %s %s (exit %s)\n' "$profile" "$mode" "$status" >&2
    failures=$((failures+1))
  fi
done
done
"${manifest[@]}" finish "$root" "$results" "$scratch" || failures=$((failures+1))
((failures == 0))
