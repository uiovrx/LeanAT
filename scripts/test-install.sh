#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build=${1:-"$root/build/linux-debug-on"}
systemc=${2:-"$root/.deps/systemc-linux-install"}
build=$(realpath -- "$build")
systemc=$(realpath -- "$systemc")
[[ -f $build/CMakeCache.txt && -f $build/cmake_install.cmake ]] || {
  echo 'Build the project with SystemC before the install smoke test.' >&2; exit 1;
}
scratch=$(mktemp -d "$build/.install-smoke.XXXXXXXX")
cleanup() {
  local resolved
  resolved=$(realpath -- "$scratch")
  case "$resolved" in "$build"/.install-smoke.*) rm -rf -- "$resolved" ;; esac
}
trap cleanup EXIT
cmake --install "$build" --prefix "$scratch/prefix"
mv -- "$scratch/prefix" "$scratch/relocated"
mkdir -- "$scratch/consumer"
cat > "$scratch/consumer/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(LeanATInstalledConsumer LANGUAGES CXX)
find_package(LeanAT 0.1 CONFIG REQUIRED COMPONENTS Runtime Stdlib SystemC)
add_executable(installed_consumer main.cpp)
target_link_libraries(installed_consumer PRIVATE LeanAT::Runtime LeanAT::Stdlib LeanAT::SystemC)
CMAKE
cat > "$scratch/consumer/main.cpp" <<'CPP'
#include <leanat/memory.hpp>
#include <systemc>
int sc_main(int, char**) {
    auto memory = leanat::Memory::make(16);
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
    return memory && memory.value().size() == 16 ? 0 : 1;
}
CPP
cmake -S "$scratch/consumer" -B "$scratch/consumer-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_PREFIX_PATH=$scratch/relocated;$systemc"
cmake --build "$scratch/consumer-build" --parallel 2
"$scratch/consumer-build/installed_consumer"
echo 'Installed, relocated Runtime + Stdlib + SystemC consumer compiled, linked, and ran.'
