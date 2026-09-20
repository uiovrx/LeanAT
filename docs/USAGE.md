# Usage guide

## 1. Prepare the environment

Run the commands in Bash from the repository root. Ubuntu 24.04 x86_64 is the release validation platform. The bootstrap also selects aarch64 Lean archives, but that does not constitute a tested aarch64 release. For Windows, use WSL 2 and a Linux filesystem checkout; no particular distribution name, username, or home directory is required.

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build curl ca-certificates git tar zstd python3 libgmp-dev
bash scripts/bootstrap.sh --jobs 4
case "$(uname -m)" in
  x86_64) lean_platform=linux ;;
  aarch64|arm64) lean_platform=linux_aarch64 ;;
  *) echo "Unsupported bootstrap architecture"; exit 1 ;;
esac
export PATH="$PWD/.deps/lean-4.34.0-$lean_platform/bin:$PATH"
export LEANAT_LAKE="$PWD/.deps/lean-4.34.0-$lean_platform/bin/lake"
```

Requirements include C++17, CMake 3.20 or newer, Ninja, Python 3.10 or newer, Bash, and the standard Unix utilities. Python tools use the standard library. Bootstrap installs pinned Lean and static C++17 SystemC under `.deps/`, verifies downloaded archive digests, and records the actual compiler/CMake/Python environment in `.deps/linux-toolchain-lock.json`. It does not install system packages or change the global Lean toolchain. Repeat the exports in a new shell before running individual Lean commands.

## 2. Build with SystemC

```sh
lake build
cmake -S . -B build/linux-debug-on -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEANAT_WITH_SYSTEMC=ON \
  -DLEANAT_BUILD_TESTS=ON \
  -DLEANAT_BUILD_CONFORMANCE=ON \
  -DCMAKE_PREFIX_PATH="$PWD/.deps/systemc-linux-install"
cmake --build build/linux-debug-on --parallel 4
ctest --test-dir build/linux-debug-on --output-on-failure \
  --exclude-regex '^leanat_original_conformance$'
```

This checks the C++ and SystemC tests. Use `bash scripts/verify.sh --jobs 4` for the complete ordered verification, including fresh facade evidence before strict conformance. Calling the strict gate before generating its required evidence will fail.

For native-only development:

```sh
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/core --parallel 4
ctest --test-dir build/core --output-on-failure
```

Native-only success does not validate the SystemC path.

## 3. Generate and run a first model

`examples/MemoryCLI.lean` defines a finite byte-state handler and connects its model and source bundle to `LeanAT.Compiler.runCLI`:

```lean
at_component ByteMemory where
  state cell : UInt8 := 0
  on internal.write cmd do
    set cell := 42
    return
```

This example is a single service segment, not a complete timed TLM memory target. Compile and run its generated SystemC smoke executable:

```sh
lake env lean --run examples/MemoryCLI.lean check
lake env lean --run examples/MemoryCLI.lean emit build/generated-memory
cmake --install build/linux-debug-on --prefix "$PWD/build/install"
cmake -S build/generated-memory -B build/generated-memory-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/build/install;$PWD/.deps/systemc-linux-install"
cmake --build build/generated-memory-build --parallel 4
./build/generated-memory-build/leanat_generated_smoke
```

Emission requires a new output directory and refuses to overwrite an existing package. Use a new directory name when iterating. The package includes generated C++, its embedded descriptor, the standalone binary descriptor, source maps, opcode metadata, and a manifest. A successful smoke exit checks that package's execution; the generated manifest does not automatically assert release acceptance.

The Python CLI wraps the same model entrypoint:

```sh
python3 -m tools.leanat check examples/MemoryCLI.lean
python3 -m tools.leanat dump-model-ir examples/MemoryCLI.lean
python3 -m tools.leanat dump-exec-ir examples/MemoryCLI.lean
python3 -m tools.leanat inventory --out build/requirement-inventory.json
```

## 4. Run timed and transport simulations

After the SystemC build, execute all generated Core/Ext facade fixtures:

```sh
LEANAT_CPP_BUILD="$PWD/build/linux-debug-on" bash scripts/test-facade.sh
```

This builds and runs eight actual executables: scalar, wire, signal, and process for each profile. Results and their source/descriptor bindings are written to `build/facade-profiles/`. The process model preserves a local value across two waits and checks state values 41 then 42. The wire test supplies explicit host bindings and checks native socket traffic and response data.

To develop a model, start with the closest fixture:

| Model concern | Source / native host example |
| --- | --- |
| State and a finite handler | `examples/MemoryCLI.lean` |
| Timed process with two waits | `tests/lean/Facade.lean`, `tests/lean/facade_process.cpp` |
| Initiator/target hierarchy and transport | `tests/lean/Facade.lean`, `tests/lean/facade_wire.cpp` |
| Signal input/output | `tests/lean/Facade.lean`, `tests/lean/facade_signal.cpp` |
| Native lifecycle, blocking, debug, DMI | `systemc/tests/` and `tests/conformance/` |

Retain the selected profile, topology, resource limits, and provider signatures when adapting a fixture. Map the intended system into ModelIR, compile it for the selected profile, supply `HostBindings` for required services, start the intended handler/process, and advance `sc_start` far enough to execute its events. The generic generated smoke entrypoint uses only a zero-time kernel start; it is not a replacement for a timed application testbench.

For bounded forward/reverse transport capture and comparison:

```sh
python3 -m tools.leanat transport-capture examples/transport-forward.json \
  --cpp-runtime build/linux-debug-on/leanat_transport_replay \
  --systemc build/linux-debug-on/systemc/leanat_systemc_transport_replay \
  --out build/transport-forward.json
```

The two native executables are distinct. A scalar descriptor result alone is insufficient evidence for transport compatibility. Capture/replay supports its declared bounded environment and rejects unsupported observations instead of silently widening its claim.

## 5. Use an installed CMake package

```cmake
cmake_minimum_required(VERSION 3.20)
project(MySimulation LANGUAGES CXX)
find_package(LeanAT 0.1 CONFIG REQUIRED COMPONENTS Runtime Stdlib SystemC)
add_executable(my_sim main.cpp)
target_link_libraries(my_sim PRIVATE LeanAT::Runtime LeanAT::Stdlib LeanAT::SystemC)
```

Configure the consumer with both the LeanAT installation and SystemC installation in `CMAKE_PREFIX_PATH`. A SystemC executable supplies `sc_main`. `scripts/test-install.sh` verifies installation relocation with an independent CMake consumer.

## Troubleshooting

- Missing Lean imports: export the pinned toolchain and run `lake build` in this checkout. Do not copy `.lake` from another operating system.
- Missing `SystemCLanguage`: bootstrap dependencies and pass `.deps/systemc-linux-install` in `CMAKE_PREFIX_PATH`.
- Failed descriptor/native profile validation: check capabilities, type layouts, byte order, socket BUSWIDTH, and provider bindings. Do not bypass validation.
- `NotRun` in acceptance: inspect `build/linux-debug-on/tests/conformance/results.json`, run the full verifier, and provide the missing backend or fixture. `--allow-incomplete` is a diagnostic option, not a passing release gate.
- Resource or fuel exhaustion: review the model's finite budgets and preserve the reported stop reason. An exhausted run is not a completed simulation.
- Stale tool fixtures: benchmark/differential inputs bind generated artifact hashes. Regenerate them for the current source and toolchain; historical hashes do not prove current behavior.
