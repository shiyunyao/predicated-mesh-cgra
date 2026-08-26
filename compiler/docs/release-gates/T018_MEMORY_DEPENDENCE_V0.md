# T018 Memory Dependence V0 Release Gate

Status: functional closure complete locally; hosted feature-head, PR merge-ref,
and post-merge `main` evidence remain to be recorded.

Base `main` after T017:
`ab06140f01e6966367fbef8bfd8a04b77fc5f20b`

Feature branch: `compiler/memory-dependence-v0`

Final feature SHA: `<PENDING_FEATURE_HEAD>`

## Frozen Address Contract

The LLVM memory frontend accepts naturally aligned, non-volatile, non-atomic
`i32` accesses in address space zero. A loop-invariant LLVM pointer root becomes
one Generic `i32` `ExternalValue` whose invocation value is a logical scratchpad
word base. It is not a host pointer, byte address, bank, LSU port, or physical
target address.

GEP offsets are interpreted with LLVM `DataLayout`. The accepted V0 address is
an exact affine word expression:

```
base + constant_offset_words + iteration_stride_words * iteration
```

The equivalent arithmetic remains present in the Generic DFG. The memory
analysis artifact does not replace executable address dataflow. Pointer PHIs,
pointer Selects, pointer chasing, non-affine indexing, subword/unaligned access,
non-default address spaces, volatile operations, atomics, and conditional Loads
remain structured failures.

The C smoke compiles with a 32-bit LLVM DataLayout. This avoids introducing a
frontend cast feature merely to bridge a 64-bit host pointer index into the
frozen 32-bit word-address ABI.

## Dependence Policy

Memory edges remain ordering-only Generic semantics and never receive a mesh
transport route.

- `Store -> Load` is RAW, `Load -> Store` is WAR, and `Store -> Store` is WAW.
- Load/Load pairs have no MemoryEdge.
- BasicAA `NoAlias` pairs have no MemoryEdge.
- Same-base, equal-stride exact affine pairs use independently solved same-
  iteration and positive cross-iteration distances. Tests cover distance zero,
  one, and two.
- Invariant Stores receive a self-WAW distance-one edge. `Store A[i]` does not.
- Remaining ordered MayAlias pairs receive both the source-order distance-zero
  edge and reverse distance-one edge.
- Unordered MayAlias accesses in mutually exclusive paths are rejected. Two
  opposite-arm Stores are also rejected because T017 V0 exposes one normalized
  Store predicate polarity; T018 does not add predicate algebra.

Predicated Stores are analyzed as potential writes. Their PredicateEdge controls
commit semantics; their MemoryEdges independently preserve potential memory
ordering.

## Ownership Boundaries

T018 is the sole producer of source-program memory dependence edges. The
temporary T017 source-Store self-WAW rule has been removed from predication
lowering and is now derived by the memory analyzer. T014 ABI output Stores retain
their independent ABI-owned self-WAW rule.

The frontend does not read `TargetModel`, scratchpad depth, bank/port placement,
or the ABI output address. Dynamic user memory combined with an ABI scalar
LiveOut is rejected in V0 because the frontend cannot prove the dynamic
footprint avoids T014's target-derived output region.

## Independent Verification

`LLVMFrontendVerifier` independently rescans LLVM Loads, Stores, pointer roots,
GEP/DataLayout offsets, and SCEV recurrences. It separately reconstructs BasicAA
classification, affine distances, and conservative fallback edges; it does not
call the production `analyzeMemoryDependences()` producer.

The corruption corpus rejects:

- missing RAW and conservative reverse edges;
- RAW/WAR kind changes and distance-one/distance-two corruption;
- spurious Load/Load and NoAlias edges;
- missing invariant-Store self-WAW and false `Store A[i]` self-WAW;
- GEP constant offset and stride corruption;
- pointer-base substitution.

## Backend Preservation

The backend regression checks LLVM recurrence memory through Generic DFG,
TargetLegalizer, MII analysis, ModuloMapper, and ModuloMappingVerifier. It proves
the RAW distance-one edge remains an ordering-only dependence with no transport.

The vector address fanout exposed a generic mapper search-quality issue: eager
placement of consumers and stage-wrapping slot choices produced many legal
modulo mappings that failed stage/RF feasibility. Candidate selection now
prefers ready distance-zero inputs, candidate ordering penalizes avoidable stage
wraps, and positive-distance neighbors receive deterministic slot affinity. A
non-LLVM production-pipeline regression verifies that RF-infeasible candidates
are rejected before an RF-feasible mapping is found. No LLVM value, fixture
name, PE, route, register, or target-specific placement is used by the fix.

## Local Evidence

- `clang-format-14 --dry-run --Werror`: PASS for all changed compiler sources.
- Clean warnings-as-errors Debug build: PASS.
- Clean Debug CTest: PASS, 40/40.
- Clean ASan/UBSan unit and semantic CTest: PASS, 38/38.
- `make llvm-memory-e2e`: PASS.
- C -> clang -> canonical LLVM -> frontend memory smoke: PASS.
- Vector add at bases 0/64/128: committed values `[11, 22, 33, 44]`,
  Golden/RTL equality PASS.
- Vector base poison: relocated bases and output addresses, Golden/RTL equality
  PASS.
- Vector data poison: changed input image changes all output data, Golden/RTL
  equality PASS.
- RAW recurrence: generated `Store -> Load`, RAW, distance one.
- RAW recurrence trip counts 1/4/7: Store counts 1/4/7 and final values 6/9/12,
  Golden/RTL equality PASS.
- Generic DFG hashes are stable across invocation poison; manifests change.

The memory oracle reads committed user Store addresses/data from Golden and RTL
traces and rejects ABI output Stores in these fixtures. Trace address fields are
decoded according to the RTL trace's unprefixed hexadecimal format; Store data
is decoded as decimal.

## Hosted Evidence

Feature-head evidence:

- compiler-fast: `<PENDING>`
- hardware-regression: `<PENDING>`

PR merge-ref evidence:

- merge-ref SHA: `<PENDING>`
- compiler-fast: `<PENDING>`
- hardware-regression: `<PENDING>`

Post-merge `main` evidence:

- merged-main SHA: `<PENDING>`
- compiler-fast: `<PENDING>`
- hardware-regression: `<PENDING>`

The hardware workflow includes retained RTL, shared-scratchpad, modulo-loop,
T013 compiler E2E, T014 Kernel ABI E2E, T015 frontend E2E, T016 recurrence E2E,
T017 predication E2E, and `make llvm-memory-e2e`.

## Known V0 Limitations

V0 does not support subword or vector memory, volatile/atomic semantics,
non-default address spaces, pointer chasing, non-affine indexing, general
path-sensitive memory ordering, predicate-disjoint dependence optimization,
conditional Load suppression, automatic scratchpad allocation, DMA, host
pointers, bank/port assignment, or dynamic-memory kernels with ABI scalar
LiveOuts.

## Release Decision

**STOP T-COMP-019 pending exact feature-head, merge-ref, and post-merge hosted
gates.**

After all immutable hosted SHAs and run IDs above are green, this section becomes:

**T-COMP-018: CLOSED**

**GO T-COMP-019**
