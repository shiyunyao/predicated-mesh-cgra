# T014 Kernel ABI V0 Final Closure

This record covers the invocation-bound Kernel ABI V0 release gate. The ABI
specializes one concrete invocation into the existing T001-T013 backend; it
does not add a runtime argument port or patch an already-generated manifest.

## Candidate

- Branch: `compiler/kernel-abi-v0`
- Baseline: `6bd7f9b91beff584ff3f643e150ef2ed800daf6d`
- Source release SHA: `15886fc13abe999041ec623c55d97c0b19c096f8`
- Evidence-document follow-up: metadata-only commit after the source gate

## Implemented closure

- Live-outs are copied and sorted by `LiveOutId` before reserving the top of
  scratchpad; the source DFG is never reordered or modified in place.
- Invocation scratchpad values are range-checked before conversion to the
  target word width. External scalar addresses are checked after
  specialization, so they cannot enter the ABI-owned output region.
- `KernelABIVerifier` independently checks target-derived depth/layout,
  specialized constant type and bits, output ordering and address constants,
  source edge/dependence preservation, ABI Store provenance, and reserved
  memory accesses.
- Recurrence boundary constants use the mapped transport/storage template and
  the verified physical RF allocation. Boundary-only write-port assignments
  are finite semantic instances; no ABI route or register is introduced.
- The layout-aware checker resolves output names through
  `KernelSignature` and `KernelABILayout` instead of hard-coding a physical
  address.

## Test matrix

| Area | Evidence | Result |
| --- | --- | --- |
| ABI unit/semantic tests | `cgra-kernel-abi-tests` | PASS (14/14) |
| Compiler debug CTest | `ctest --test-dir compiler/build/ci-debug` | PASS (34/34) |
| Compiler sanitizer CTest | `ctest --test-dir compiler/build/ci-sanitize` | PASS (34/34) |
| T013 generated-program E2E | `make compiler-e2e` | PASS (golden/RTL/semantic observations) |
| Scalar input poison | `make kernel-abi-scalar-e2e` | PASS (`x=7 -> 14`, `x=9 -> 18`) |
| Base-address poison | `make kernel-abi-base-load-e2e` | PASS (addresses 17 and 23) |
| Recurrence seed poison | `make kernel-abi-recurrence-e2e` | PASS (seed 5 -> 9, seed 20 -> 24) |
| Trip-count variation | `make kernel-abi-tripcount-e2e` | PASS (trip counts 1 and 7; fresh manifests) |
| Full ABI hardware matrix | `make kernel-abi-e2e` | PASS (all cases use generated manifests and Verilator) |
| Retained RTL regression | `make regression` | PASS |
| Shared scratchpad regression | `make shared-scratchpad-tests` | PASS |
| Modulo-loop regression | `make modulo-loop` | PASS (14 Python tests plus RTL replay) |

Named ABI negative regressions include:

- `KernelABI.RejectsMissingInputAndReservedScratchpadCollision`
- `KernelABI.InvocationRejectsUnknownScalarInput`
- `KernelABI.InvocationRejectsDuplicateScalarInput`
- `KernelABI.InvocationRejectsZeroTripCount`
- `KernelABI.InvocationRejectsScalarWidthOverflow`
- `KernelABI.InvocationRejectsInvalidPredicateBits`
- `KernelABI.InvocationRejectsScratchpadAddressOutOfRange`
- `KernelABI.InvocationRejectsConflictingDuplicatePreload`
- `KernelABI.RejectsScratchpadValueOverflowBeforeNarrowing`
- `KernelABI.RejectsUnsupportedLiveOutType`
- `KernelABI.LiveOutLayoutIsSortedById`

The verifier-corruption path is covered by
`KernelABI.BindsInputsAndLiveOutsWithoutChangingSource`. Unresolved boundary
and external-provider handling is covered by
`testRecurrenceBoundaries` in `compiler/tests/IR/DFGVerifierTest.cpp` and
`testExternalProviderIsExplicitFailure` in
`compiler/tests/Lowering/TargetLoweringTest.cpp`. Constant-capacity failure is
covered by `testConstantCapacityFailure` in the same lowering test, which
requires an explicit allocator failure rather than aliasing semantic values.

## Hosted evidence

The compiler-fast and hardware-regression workflows trigger on
`compiler/kernel-abi-v0`; the hardware job explicitly runs
`make kernel-abi-e2e`. The source release SHA above was pushed and both exact
feature-head gates completed successfully.

- Exact feature-head `compiler-fast-gate`: PASS ([run 32865493201](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32865493201))
- Exact feature-head `hardware-regression-gate`: PASS ([run 32865493227](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32865493227)); the job ran retained RTL, modulo-loop, T013 compiler E2E, and all four T014 ABI E2E targets.

## Known V0 limitations

Kernel ABI V0 is invocation-bound and requires a positive concrete trip count.
It intentionally has no LLVM frontend, runtime symbolic trip count, host
runtime, DMA ABI, automatic scratchpad allocator, predicate-to-integer
live-out conversion, rotating registers, spills, multicast routing, or MVE.

## Release decision

`T-COMP-014: CLOSED`

`GO T-COMP-015`

The source release SHA and hosted run IDs are recorded above after the final
release-seal push. The evidence-document follow-up is metadata-only and does
not change compiler or hardware behavior.
