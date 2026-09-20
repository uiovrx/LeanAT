# Project overview

## Purpose

LeanAT explores a model-driven route to approximately timed transaction-level simulation: write a bounded model, inspect and validate its intermediate representation, generate a SystemC-facing implementation, and compare observable results across independent execution paths. It targets explicit resource budgets and inspectable behavior rather than unrestricted translation of arbitrary C++ or Lean programs.

The two semantic profiles are `AT-Core-1.1-draft` and `AT-Ext-1.1-draft`. Core supplies transactional state, event scheduling, transport, processes, ownership, and lifecycle behavior. Ext adds bounded structured operations and optional services. Descriptor capabilities and host bindings determine which features an individual model can actually execute.

## Architecture

```text
Lean model / supported frontend syntax
              |
              v
         typed ModelIR --------> source/reference evaluation
              |
        validation + lowering
              v
         typed ExecIR ---------> independent ExecIR evaluation
              |
       checked binary descriptor
              |
        C++ interpreter + runtime
              |
     SystemC host adapters + generated facade
              |
       native TLM sockets / SystemC kernel
```

The runtime represents time using integer ticks and logical turns. Event ordering, finite pump slices, transactional prepare/commit behavior, generation-checked handles, and owning result values make scheduler and lifetime decisions explicit. C++ runtime headers are independent of SystemC; the optional `LeanAT::SystemC` target supplies the kernel and TLM boundary.

The standard library includes memory, register banks, queues, resources, pipelines, crossbars, timers, payload helpers, and structured process services. Availability of a C++ service does not imply that every frontend spelling or generated provider binding exists. Use the executable examples and the validated provider signatures as the contract for a particular model.

## SystemC compatibility contract

The supported native profile uses a little-endian host and 32-bit TLM socket BUSWIDTH. The abstract address space remains 64-bit. Unsupported native byte order and socket widths are rejected. Some frontend examples intentionally exercise broader IR declarations; successful IR validation alone does not establish native SystemC support.

`TimeCodec` converts integer ticks to exact SystemC resolution units, checks overflow and grid alignment, and rejects deadlines in the past. Set kernel time resolution before constructing model time codecs. Host and model time units must agree.

The adapters exercise real forward/backward nonblocking callbacks, phase registration, return annotations, payload snapshots, response writeback, memory-manager pins, blocking/debug paths, signals, event pumping, and raw DMI services. Nonblocking generic payloads require a memory manager. Host buffers, callbacks, and providers must obey the declared ownership and lifetime rules.

Generated hierarchical components share a `RuntimeHostAdapter` and its event pump. Explicit `HostBindings` connect provider behavior and native transport. The standalone `RuntimeDomain` helper used by scalar fixtures is not the integrated runtime scheduler. For transport integration, start from `tests/lean/Facade.lean` and `tests/lean/facade_wire.cpp`, not from a scalar-only descriptor example.

Raw DMI exposes host memory. External code must honor permissions, invalidation, and backing-storage lifetime; arbitrary third-party loads/stores cannot be fully observed or replayed. A declared protocol adapter checks a finite relation, but does not prove the behavior of an arbitrary external target.

## Structured service argument layouts

These finite owning `Value` layouts are bound through `StructuredServices`. Descriptor types and provider signatures must match.

| Operation | Arguments | Result |
| --- | --- | --- |
| ScopeNew / ScopeClose | parent or scope | scope / Unit |
| ScopeTransfer | object, from, to | Unit |
| ScopeCancel | scope, reason Bytes | Unit |
| SpawnProcess | scope, argument Record | task handle |
| TrySpawnProcess / SubmitTask | scope, argument Record | Except: task or recoverable error |
| RequestTaskSlot | process, scope | Except: ticket or recoverable error |
| CancelTask | task, reason Bytes | Unit |
| TaskResultGet / TaskResultRelease | task, creator scope | owning TaskOutcome / Unit |
| WaitGroupNew | process, scope, mode, count, policy, result TypeId, result bytes | registered wait handle |
| WaitArm | registered wait, ordinal, priority, task, creator scope | Unit |
| WaitResultGet / WaitGroupRelease | registered wait, owner scope | owning aggregate / Unit |

Descriptor v5 process policy uses caller-supplied scope ownership and retains results until explicit release. Completing execution frees its frame; outstanding consumers and pins retain independent result rights. Configured capacities and overflow policies are part of the model, not unlimited host allocation promises.

## What the evidence establishes

The original acceptance gate retains 30 Core IDs and 40 Ext IDs. E-T39 compares the declared opcode corpus across ModelIR, ExecIR, native Runtime, and SystemC; E-T40 compares Core-selected and Ext-selected executions of the Core corpus. Freshly generated facade simulations also cover scalar, wire, signal, and suspended/resumed process behavior.

These checks do not establish a whole-pipeline refinement theorem, arbitrary host-callback correctness, every scheduler/numeric combination, or complete IEEE SystemC/TLM conformance. The 598 module-level acceptance entries retained in the requirement catalog are an inventory; they are not 598 independently passing tests. See [validation](VALIDATION.md) for exact commands and the public release's measured results.

The Python compiler CLI currently rejects requests to enable its unsupported managed-access or raw-DMI plumbing. Their lower-level providers and dedicated tests have a narrower, explicit integration contract. Proof artifacts are similarly scoped: a successful simulation or descriptor check does not manufacture a proof certificate.
