#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
case $(uname -m) in
  x86_64) lean_platform=linux ;;
  aarch64|arm64) lean_platform=linux_aarch64 ;;
  *) echo 'Unsupported Linux architecture' >&2; exit 2 ;;
esac
lake=${LEANAT_LAKE:-"$root/.deps/lean-4.34.0-$lean_platform/bin/lake"}
[[ -x $lake ]] || { echo 'Run bash scripts/bootstrap.sh first' >&2; exit 1; }
cd -- "$root"
failures=0
if ! "$lake" build LeanAT.Frontend.BlockSyntax LeanAT.Protocol.Builtins LeanAT.ModelIR.Process LeanAT.Compiler.Main; then
  echo 'FAIL frontend dependency build' >&2
  failures=$((failures+1))
fi
for file in tests/lean/Frontend.lean tests/lean/Sidebands.lean tests/lean/Processes.lean tests/lean/SourceProvenance.lean examples/Memory.lean examples/Protocol.lean; do
  echo "Frontend acceptance: $file"
  if ! "$lake" env lean "$file"; then
    echo "FAIL frontend acceptance: $file" >&2
    failures=$((failures+1))
  fi
done
if ! "$lake" env lean --run tests/lean/E39Frontend.lean; then
  echo 'FAIL native E39 frontend execution' >&2
  failures=$((failures+1))
fi
if ! "$lake" env lean --run examples/Process.lean run; then
  echo 'FAIL native process two-await reference' >&2
  failures=$((failures+1))
fi
if ! "$lake" env lean --run examples/Transport.lean check; then
  echo 'FAIL native transport hierarchy compilation' >&2
  failures=$((failures+1))
fi
# Match the checked rejection reason, not merely a nonzero compiler status.
cases=(CoreProtocol.lean UnknownMember.lean TimedAwait.lean ReturnThenSchedule.lean UnknownHandler.lean RequiresFalse.lean HandlerAwaitSlot.lean DoubleAwait.lean TransportAwait.lean UnpreparedTransport.lean)
patterns=('^MissingCapability:' '^unexpected identifier; expected command$' '^UnsupportedSyntax:' '^StatementAfterReturn\b' '^UnknownHandlerEndpoint\b' '^Tactic `decide` proved that the proposition\n\s+False\nis false' '^AwaitSlotForbiddenInContext\b' '^WaitAlreadyConsumed\b' '^AwaitForbiddenInContext\b' '^UnpreparedTransportResponse\b')
for index in "${!cases[@]}"; do
  if output=$("$lake" env lean "tests/lean/fail/${cases[$index]}" 2>&1); then
    printf 'Expected rejection but compilation passed: %s\n' "${cases[$index]}" >&2
    failures=$((failures+1))
    continue
  fi
  # Parse payloads only from diagnostics attributed to this exact fixture.
  # A matching word in a path or an unrelated dependency error is not evidence.
  if ! python3 -c '
import re, sys
fixture, expected = sys.argv[1:]
output = sys.stdin.read()
infrastructure = r"object file .* does not exist|unknown module prefix|invalid import|failed to (load|open)|cannot (load|find).*module|segmentation fault|stack overflow|panic|uncaught exception|internal error|error:.*(no such file|permission denied)"
if re.search(infrastructure, output, re.IGNORECASE):
    raise SystemExit(1)
header = re.compile(r"^" + re.escape(fixture) + r":\d+:\d+: error(?:\([^)]*\))?:\s*(.*)$")
any_header = re.compile(r"^.*:\d+:\d+: (?:error|warning|information)(?:\([^)]*\))?:")
payloads, current = [], None
for line in output.splitlines():
    match = header.match(line)
    if match:
        if current is not None: payloads.append("\n".join(current))
        current = [match.group(1)]
    elif any_header.match(line):
        if current is not None: payloads.append("\n".join(current))
        current = None
    elif current is not None:
        current.append(line)
if current is not None: payloads.append("\n".join(current))
raise SystemExit(0 if any(re.search(expected, payload) for payload in payloads) else 1)
' "tests/lean/fail/${cases[$index]}" "${patterns[$index]}" <<< "$output"; then
    printf 'Wrong rejection for %s:\n%s\n' "${cases[$index]}" "$output" >&2
    failures=$((failures+1))
    continue
  fi
  printf 'PASS reject %s\n' "${cases[$index]}"
done
if ((failures)); then
  printf 'Frontend failed: %s case(s).\n' "$failures" >&2
  exit 1
fi
