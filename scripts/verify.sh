#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
jobs=4
configuration=Debug
with_systemc=ON
while (($#)); do
  case "$1" in
    --jobs) jobs=${2:?--jobs requires a number}; shift 2 ;;
    --configuration) configuration=${2:?--configuration requires Debug or Release}; shift 2 ;;
    --without-systemc) with_systemc=OFF; shift ;;
    --help) echo 'Usage: bash scripts/verify.sh [--jobs N] [--configuration Debug|Release] [--without-systemc]'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'Jobs must be positive' >&2; exit 2; }
[[ $configuration == Debug || $configuration == Release ]] || { echo 'Configuration must be Debug or Release' >&2; exit 2; }
case $(uname -m) in
  x86_64) lean_platform=linux ;;
  aarch64|arm64) lean_platform=linux_aarch64 ;;
  *) echo 'Unsupported Linux architecture' >&2; exit 2 ;;
esac
export LEANAT_LAKE="$root/.deps/lean-4.34.0-$lean_platform/bin/lake"
export PATH="$(dirname -- "$LEANAT_LAKE"):$PATH"
cd -- "$root"
summary=()
failures=0
last_gate_status=0
run_gate() {
  local name=$1
  shift
  printf '\n=== %s ===\n' "$name"
  if "$@"; then
    last_gate_status=0
    summary+=("PASS $name")
  else
    last_gate_status=$?
    failures=$((failures+1))
    summary+=("FAIL $name (exit $last_gate_status)")
  fi
}
skip_gate() { summary+=("SKIP $1 ($2)"); }
check_lean() {
  [[ -x $LEANAT_LAKE ]] || { echo 'Run bash scripts/bootstrap.sh first' >&2; return 1; }
  "$root/.deps/lean-4.34.0-$lean_platform/bin/lean" --version | grep -E 'version 4\.34\.0([ ,()]|$)'
}
# Use a Linux-only build path; never reuse a copied Windows CMake cache.
build="$root/build/linux-${configuration,,}-${with_systemc,,}"
args=(-S "$root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$configuration"
      -DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON -DCMAKE_CXX_EXTENSIONS=OFF
      -DLEANAT_BUILD_TESTS=ON -DLEANAT_BUILD_CONFORMANCE=ON "-DLEANAT_WITH_SYSTEMC=$with_systemc")
configure_cpp() {
  local python systemc
  python=$(command -v python3) || { echo 'Python3 is required for conformance' >&2; return 1; }
  local configure_args=("${args[@]}" "-DPython3_EXECUTABLE=$python")
  if [[ $with_systemc == ON ]]; then
    systemc="$root/.deps/systemc-linux-install"
    [[ -f $systemc/.leanat-verified-sha256 ]] || { echo 'Bootstrap the pinned Linux SystemC first' >&2; return 1; }
    configure_args+=("-DCMAKE_PREFIX_PATH=$systemc" "-DSystemCLanguage_DIR=$systemc/lib/cmake/SystemCLanguage")
  fi
  cmake "${configure_args[@]}"
}
lean_build_ok=0
process_policy_ok=0
run_gate 'Lean toolchain' check_lean
if ((last_gate_status == 0)); then
  run_gate 'Lean library build' "$LEANAT_LAKE" build
  if ((last_gate_status == 0)); then lean_build_ok=1; fi
  # Fixtures remain independent gates; a failed build remains an overall failure.
  shopt -s nullglob
  positive_tests=(tests/lean/*.lean)
  if ((${#positive_tests[@]} == 0)); then run_gate 'Lean fixture inventory' false; fi
  for file in "${positive_tests[@]}"; do
    case "$file" in tests/lean/Frontend.lean|tests/lean/Sidebands.lean) continue ;; esac
    run_gate "Lean $file" "$LEANAT_LAKE" env lean "$file"
  done
  run_gate 'Lean ModelMain execution' "$LEANAT_LAKE" env lean --run tests/lean/ModelMain.lean
  for fixture in Compiler Hierarchy ExecProcess TransportCLI SourceMap OpcodeCoverage ModelNodeCoverage; do
    run_gate "Lean $fixture execution" "$LEANAT_LAKE" env lean --run "tests/lean/$fixture.lean"
  done
  if ((lean_build_ok)); then
    for fixture in ReferenceStorage ReferenceObjects ReferenceManaged ReferenceStructured ReferenceJson ReferenceRunner ReferenceAllocatorPolicy ReferenceResume ReferenceInstance ReferenceControlFlow ProcessPolicy E39Frontend CoreReference OpcodeStorage OpcodeProtocol OpcodeCore; do
      run_gate "Lean $fixture actual reference execution" "$LEANAT_LAKE" env lean --run "tests/lean/$fixture.lean"
      if [[ $fixture == ProcessPolicy ]] && ((last_gate_status == 0)); then process_policy_ok=1; fi
    done
    if [[ -f tests/lean/ReferenceRuntime.lean ]]; then
      run_gate 'Lean ReferenceRuntime actual reference execution' "$LEANAT_LAKE" env lean --run tests/lean/ReferenceRuntime.lean
    fi
  else
    skip_gate 'Lean actual reference executions' 'Lean dependency build failed; do not execute references against mixed artifacts'
  fi
  run_gate 'Lean frontend acceptance and rejection' bash tests/lean/run-frontend.sh
else
  skip_gate 'Lean build, fixtures and frontend' 'Lean toolchain unavailable'
fi
run_gate 'Python unittest' python3 -m unittest discover -s tests/python -v
run_gate 'Conformance comparator unittest' python3 -B -m unittest discover -s tests/conformance -p 'test_*.py' -v
run_gate 'C++ configure' configure_cpp
if ((last_gate_status == 0)); then
  run_gate 'C++ build' cmake --build "$build" --parallel "$jobs"
  if ((last_gate_status == 0)); then
    if ((process_policy_ok)); then
      run_gate 'C++ process policy: original and shared compiled fixtures' "$build/test_process_policy" \
        "$root/build/compiler-fixtures/process-policy-v5.execir.bin" \
        "$root/build/compiler-fixtures/process-policy-shared-v5.execir.bin"
    else
      skip_gate 'C++ compiled process policy fixtures' 'ProcessPolicy execution failed or unavailable; do not consume stale fixtures'
    fi
    run_gate 'CTest: C++ and SystemC' ctest --test-dir "$build" --output-on-failure --parallel "$jobs" --exclude-regex '^leanat_original_conformance$'
    if [[ $with_systemc == ON ]]; then
      run_gate 'Installed package relocation' bash scripts/test-install.sh "$build" "$root/.deps/systemc-linux-install"
      if ((lean_build_ok)); then
        run_gate 'Generated SystemC facade' env "LEANAT_CPP_BUILD=$build" bash scripts/test-facade.sh
      else
        skip_gate 'Generated SystemC facade' 'Lean library build failed'
      fi
    fi
    # Profile conformance consumes the observations from this run's generated
    # facade binaries, so collect it after those executions, not beforehand.
    run_gate 'CTest: strict original conformance' ctest --test-dir "$build" --output-on-failure --tests-regex '^leanat_original_conformance$'
  else
    skip_gate 'CTest' 'C++ build failed; do not test stale binaries'
  fi
else
  skip_gate 'C++ build and CTest' 'C++ configure failed; do not reuse stale cache'
fi
printf '\n=== Linux verification summary ===\n'
printf '%s\n' "${summary[@]}"
if ((failures)); then
  printf 'Linux verification failed: %s gate(s), SystemC=%s, conformance=ON.\n' "$failures" "$with_systemc" >&2
  exit 1
fi
echo "Linux verification passed (SystemC=$with_systemc, conformance=ON)."
