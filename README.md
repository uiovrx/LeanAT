# LeanAT

LeanAT is an experimental toolchain for bounded, approximately timed (AT) models. It combines a Lean 4 modeling frontend and reference semantics with a C++17 runtime and real SystemC/TLM adapters. Models lower through typed ModelIR and ExecIR into checked binary descriptors and generated SystemC components.

The project aims to preserve modeled behavior across the Lean reference, native runtime, and SystemC execution paths. Its executable acceptance corpus covers the `AT-Core-1.1-draft` and `AT-Ext-1.1-draft` profiles. Passing that corpus is finite regression evidence, not universal equivalence or IEEE certification.

- [Project overview, architecture, and compatibility boundaries](docs/PROJECT.md)
- [Build, model generation, simulation, and integration guide](docs/USAGE.md)
- [Reproducible validation and release scope](docs/VALIDATION.md)

## Quick start

Use Ubuntu 24.04 on Linux or in WSL 2, with the checkout on the Linux filesystem. The release bootstrap pins [Lean 4.34.0](https://github.com/leanprover/lean4/releases/tag/v4.34.0) and [Accellera SystemC 3.0.2](https://github.com/accellera-official/systemc/releases/tag/3.0.2), and verifies archive SHA-256 values.

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build curl ca-certificates git tar zstd python3 libgmp-dev
git clone https://github.com/uiovrx/LeanAT.git
cd LeanAT
bash scripts/bootstrap.sh --jobs 4
bash scripts/verify.sh --jobs 4
```

The full verifier compiles Lean and C++, runs positive/negative frontend tests, Python tests, native/SystemC tests, a relocated installed-package consumer, eight generated SystemC simulations, and the strict 70-ID acceptance gate. The complete reference corpus can take substantially longer than the basic build; its aggregate CTest timeout is two hours. A missing backend or case fails the strict gate.

For a smaller first build and an executable generated model, follow [the usage guide](docs/USAGE.md). The native-only C++ build is useful for development, but SystemC must be enabled to check the project's interoperability goal.

## Repository contents

| Path | Purpose |
| --- | --- |
| `LeanAT/`, `LeanAT.lean` | Frontend, typed IRs, compiler, reference evaluators, and scoped proofs |
| `runtime/`, `stdlib/` | C++ runtime, bounded storage, processes, protocols, and modeling services |
| `systemc/`, `templates/` | Native SystemC adapters and generated runtime support |
| `tools/`, `schemas/` | Compiler CLI, descriptor/replay tools, and artifact schemas |
| `examples/` | Small source models and tool fixtures |
| `tests/` | Executable regressions, comparator fixtures, and original acceptance requirements |
| `scripts/`, `cmake/` | Dependency bootstrap, verification, and relocatable CMake package support |

This is a source release. Dependencies, binaries, private workspace configuration, development conversations, and historical reports are not included. [MIT license](LICENSE); downloaded dependencies retain their own licenses.
