#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
jobs=4
while (($#)); do
  case "$1" in
    --jobs) jobs=${2:?--jobs requires a number}; shift 2 ;;
    --help) echo 'Usage: bash scripts/bootstrap.sh [--jobs N]'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'Jobs must be positive' >&2; exit 2; }
[[ $(uname -s) == Linux ]] || { echo 'This bootstrap requires Linux' >&2; exit 2; }
for command in curl sha256sum tar zstd cmake ninja c++ python3; do
  command -v "$command" >/dev/null || { echo "Missing dependency: $command" >&2; exit 1; }
done
case $(uname -m) in
  x86_64) lean_platform=linux; lean_hash=caaa98356098c85dc0fcbbd28e1ec66f39eb6551829972b752ff20e1286b646b ;;
  aarch64|arm64) lean_platform=linux_aarch64; lean_hash=40b04fdb7fb849d3c80e10c3bbeebc7b7354b6d3f07450b9168c2149b40d2a82 ;;
  *) echo 'Supported architectures: x86_64, aarch64' >&2; exit 2 ;;
esac
# Lean digests: official GitHub v4.34.0 expanded_assets, verified 2026-09-20.
systemc_hash=9b3693ed286aab958b9e5d79bb0ad3bc523bbc46931100553275352038f4a0c4
deps="$root/.deps"
mkdir -p -- "$deps/downloads"
used_archives=()
scratch=$(mktemp -d "$deps/.linux-bootstrap.XXXXXXXX")
cleanup() {
  # Delete only the unique directory created by this invocation.
  case "$scratch" in "$deps"/.linux-bootstrap.*) rm -rf -- "$scratch" ;; esac
}
trap cleanup EXIT
download() {
  local url=$1 name=$2 hash=$3 archive="$deps/downloads/$2"
  if [[ ! -f $archive ]]; then
    curl --fail --location --retry 3 --output "$scratch/$name.part" "$url"
    printf '%s  %s\n' "$hash" "$scratch/$name.part" | sha256sum --check --status
    mv -- "$scratch/$name.part" "$archive"
  fi
  printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --status || {
    echo "Checksum mismatch: $archive (remove this invalid cache file before retrying)" >&2; return 1;
  }
  used_archives+=("$archive")
}
lean_dir="$deps/lean-4.34.0-$lean_platform"
if [[ ! -f $lean_dir/.leanat-verified-sha256 ]] || [[ $(cat "$lean_dir/.leanat-verified-sha256") != "$lean_hash" ]]; then
  archive="lean-4.34.0-$lean_platform.tar.zst"
  download "https://github.com/leanprover/lean4/releases/download/v4.34.0/$archive" "$archive" "$lean_hash"
  tar --zstd -xf "$deps/downloads/$archive" -C "$deps"
  printf '%s\n' "$lean_hash" > "$lean_dir/.leanat-verified-sha256"
fi
"$lean_dir/bin/lean" --version | grep -E 'version 4\.34\.0([ ,()]|$)'
"$lean_dir/bin/lake" --version
systemc_dir="$deps/systemc-linux-install"
if [[ ! -f $systemc_dir/.leanat-verified-sha256 ]] || [[ $(cat "$systemc_dir/.leanat-verified-sha256") != "$systemc_hash" ]]; then
  download 'https://github.com/accellera-official/systemc/archive/refs/tags/3.0.2.tar.gz' 'systemc-3.0.2.tar.gz' "$systemc_hash"
  tar -xzf "$deps/downloads/systemc-3.0.2.tar.gz" -C "$scratch"
  cmake -S "$scratch/systemc-3.0.2" -B "$scratch/systemc-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_CXX_EXTENSIONS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_PREFIX="$systemc_dir"
  cmake --build "$scratch/systemc-build" --parallel "$jobs"
  cmake --install "$scratch/systemc-build"
  printf '%s\n' "$systemc_hash" > "$systemc_dir/.leanat-verified-sha256"
fi
# Keep a Linux-specific lock beside local dependencies; do not overwrite the Windows lock.
python3 - "$deps/linux-toolchain-lock.json" "$lean_platform" "$lean_hash" "$systemc_hash" <<'PY'
import json, platform, subprocess, sys
output, architecture, lean_hash, systemc_hash = sys.argv[1:]
def version(command):
    return subprocess.check_output(command, text=True).splitlines()[0]
lock = {
    "lean": {"version": "4.34.0", "archive_sha256": lean_hash,
             "path": ".deps/lean-4.34.0-" + architecture},
    "systemc": {"version": "3.0.2", "archive_sha256": systemc_hash,
                "path": ".deps/systemc-linux-install", "cxx_standard": 17, "shared": False},
    "compiler": version(["c++", "--version"]), "cmake": version(["cmake", "--version"]),
    "python": platform.python_version(), "machine": platform.machine(),
}
with open(output, "w", encoding="utf-8") as stream:
    json.dump(lock, stream, indent=2)
    stream.write("\n")
PY
# Only remove exact archive paths consumed successfully by this invocation.
# On failure retain verified archives for a retry; installed dependencies and lock stay.
for archive in "${used_archives[@]}"; do
  rm -f -- "$archive"
done
printf 'Lean bin: %s/bin\nSystemC prefix: %s\nNext: bash scripts/verify.sh --jobs %s\n' "$lean_dir" "$systemc_dir" "$jobs"
