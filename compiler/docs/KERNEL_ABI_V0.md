# Kernel ABI V0

T014 defines the invocation boundary consumed by the release-green backend.  It is
invocation-bound: changing scalar values, the trip count, or the scratchpad image
recompiles the manifest.

## Boundary model

Every Generic DFG `ExternalValue` is one loop-invariant scalar input.  The binder
specializes it to one semantic `ConstantRef` per invocation, preserving the source
DFG IDs and reusing that constant for every ordinary and recurrence-boundary use.
The existing target constant allocator still chooses the physical constant-memory
address; an `ExternalValueId` is never a machine address.

Every `LiveOut` of V0 type `i32` is materialized as an ABI-owned Store to the top
of the target scratchpad.  Outputs are sorted by `LiveOutId` and occupy

```
[scratchpadDepth - liveOutCount, scratchpadDepth)
```

The Store executes once per loop iteration and has a distance-one self-WAW memory
edge.  Consequently, after the final iteration the reserved word contains the
live-out value from that iteration.  The user preload range ends at the output
region base and may not overlap it.

The ABI verifier independently checks the target-derived depth, output ordering,
address constants, Store provenance, and all concrete Load/Store addresses in the
bound DFG.  Invocation preload values are checked before narrowing to the target
word width; values outside that width are rejected.

V0 requires a positive concrete trip count.  Zero-trip and runtime-symbolic
semantics are intentionally deferred to later ABI work.

## Recurrence boundaries

The binder rewrites an `ExternalValueRef` in a recurrence boundary to the same
invocation-specialized `ConstantRef` used by ordinary operands.  T011 retains the
mapped route and RF allocation for negative producer iterations.  T012 lowers the
first boundary launch or RF write through `CONST_DATA`, `CONST_TRUE`, or
`CONST_FALSE` and the physical address from `ConstantImage`; it does not create a
prologue-only route or register.

An unresolved external boundary is an error.  Predicate seeds are supported only
when represented by target-supported predicate constants.

## Artifacts and entry points

`cgrac-compile-kernel` accepts a Generic DFG and a
`cgra.kernel_invocation.v1` JSON file, then emits the existing
`cgra.program_manifest.v1` plus `cgra.kernel_abi.layout.v1` and backend artifacts.
`compileGenericDFG` remains the lower-level backend API; `compileKernel` performs
ABI binding and then calls it without a second mapper or lowering pipeline.

The layout-aware checker in `tools/check_kernel_abi_e2e.py` resolves output names
through the generated signature and layout before inspecting golden/RTL traces;
the E2E oracle therefore does not hard-code a physical scratchpad address.

The current target has one constant-memory address selector per tile/cycle.  Two
different configuration scalars consumed by the same tile/cycle therefore remain
a target-control conflict and are rejected by T012.  V0 does not invent a runtime
scalar port or silently spill such values to scratchpad.  For recurrence boundary
instances, T010 records a source-compatible finite-boundary RF write port when it
differs from the steady-state producer port; the physical register and mapped
transport remain unchanged.

## Deliberate limitations

V0 does not provide a host runtime, arbitrary external providers, zero-trip
language semantics, predicate-to-integer live-out conversion, rotating registers,
spilling, or runtime trip-count patching.  These are separate future capabilities.
