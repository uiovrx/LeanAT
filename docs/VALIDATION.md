# Validation and source release scope

## Reproduce the acceptance check

```sh
bash scripts/bootstrap.sh --jobs 4
bash scripts/verify.sh --jobs 4
```

The verifier attempts independent gates and returns failure if any required gate fails. It does not reuse stale C++ binaries after a failed build, and strict conformance does not treat missing scenarios as passing. The expected final message is:

```text
Linux verification passed (SystemC=ON, conformance=ON).
```

Check `build/linux-debug-on/tests/conformance/results.json`: `gate_pass` must be true and `counts` must contain 70 Pass, zero Fail, and zero NotRun. E-T39 must execute all 277 declared cases and observe all 65 opcodes in both native backends. E-T40 requires the original 30 Core IDs, profile-selected executions, full published observations, and its negative controls. Eight generated Core/Ext facade runs precede that comparison.

The two Python suites currently contain 74 tool tests and 37 conformance comparator tests. Comparator fixture observations are intentionally retained under `tests/python/fixtures/`; they are input data for schema rejection tests, not a saved verdict that substitutes for current execution.

## Release verification

The release was verified locally on 2026-09-20 in an independent Ubuntu 24.04 x86_64 checkout, using GCC 13.3.0, CMake 3.28.3, Python 3.12.3, Lean 4.34.0, and SystemC 3.0.2. LeanAT's Lean and C++ artifacts were built from source; no prebuilt LeanAT outputs were copied. The pinned dependency installations were reused, with SystemC held inside the validation checkout.

The final `bash scripts/verify.sh --jobs 8` run completed successfully against the frozen publication sources, with SystemC and strict conformance enabled.

| Check | Executed result |
| --- | --- |
| Lean library, frontend acceptance/rejection, and reference executions | Pass |
| Python tool / conformance comparator suites | 74/74 and 37/37 |
| CTest | 57/57: 56 native/SystemC tests plus strict original conformance |
| Original acceptance | 70/70; zero Fail and zero NotRun |
| E-T39 | 277/277 cases; all 65 opcodes observed positively in both native Runtime and SystemC |
| E-T40 | 30 original Core requirements; 21 Core/Ext scenario pairs |
| Generated SystemC facades | 8/8 executions |
| Installed and relocated CMake package consumer | Pass |
| Documented generation, compilation, simulation, CLI, and transport-capture commands | Pass |

The E39 and E40 source-stability checks passed. After execution, all 261 source/toolchain hashes recorded by the top-level acceptance report were checked against the validation checkout. Full reports remain local build artifacts; reproduce them using the commands above. These measurements cover the declared corpus and configuration, not every possible model, host callback, or SystemC implementation.

## Why these files are included

The release preserves the compiler, reference semantics, native runtime, standard library, SystemC adapters, generation template, artifact tools, examples, and executable verification dependencies. Tests remain because removing them would prevent another user from checking the original compatibility objective.

`tests/conformance/requirements.json` retains the exact original scenario and acceptance text with stable IDs: 70 original regression entries and 598 module acceptance entries. The text remains in its original language to avoid changing the acceptance contract during publication. This compact data replaces the historical specification/review bundle. Inventory entries remain unevaluated until supported by actual evidence; the 70-ID gate does not imply 598 independent passes.

Excluded material includes downloaded dependencies, build products, caches, generated simulation artifacts, private workspace scripts, machine-specific toolchain reports, old specification copies, review discussions, and development logs. There is no dependency on the original workstation, WSL distribution name, development task, or private repository.

The public snapshot uses fresh version-control history with a neutral contributor identity. No original Git history is needed to build or run it. Apache-2.0 applies to the project source; Lean and SystemC are fetched separately and retain their upstream licenses.
