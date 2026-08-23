# Modulo Resource Model and Mapping IR

T-COMP-004 defines the finite resource-time universe consumed by later mapper
passes. It does not choose node order, placement, routes, stages, or an II.

## Finite modulo universe

`ModuloTimeDomain(ii)` requires `ii >= 1` and centralizes normalization and
advancement. Every resource is replicated over exactly slots `[0, II)`. There
is no schedule horizon, trip-count expansion, absolute cycle, or stage in the
resource model.

Transport actions use `elapsedFromProducerIssue` and may therefore have an
elapsed value greater than II. Their resource slot is derived as
`advance(producerIssueSlot, elapsed)`, so a long relative path never creates
additional resource layers.

The TargetModel distinguishes result latency from producer output readiness.
`producerOutputReadyOffset` is the first elapsed cycle at which a produced
value may enter the output/storage fabric: canonical FU results are ready at
offset 0, while a LOAD is ready at offset 2. A route must query this field; it
must not infer readiness as `resultLatency - 1`.

## Resources

The immutable `ModuloResourceModel` enumerates, in deterministic slot/row/col
order:

* one `FU(tile, slot)` for every tile;
* one `LSU(tile, slot)` only for TargetModel LSU-capable tiles;
* outgoing directional Data links for in-bounds neighbors;
* the corresponding independent Predicate links.

FU and LSU resources are separate. A FU and an LSU may therefore reserve the
same tile and modulo slot at the core-resource level. There is no global
`SharedScratchpad(slot)` resource: the four statically owned LSU resources
represent the four shared-memory access ports. Physical RF entries, RF ports,
constant memory, switchbox muxes, and input buffers are intentionally outside
this core model.

Links are directional and reserved at launch slot. A link launched at logical
cycle `t` arrives after the TargetModel mesh hop latency; V0 assumes the
canonical one-cycle link. Reverse direction and Data/Predicate domains are
different resources.

Operation footprints query TargetModel operation descriptors and per-tile
capabilities. The canonical v2 target defaults all FU operations to every tile
and uses explicit LSU tile ownership; future contracts can restrict operation
sets per tile without changing this resource API. They reserve the
operation's execution resource for `issueOccupancy` consecutive modulo slots,
including wraparound. If a footprint revisits the same resource because its
occupancy is greater than II, construction fails rather than silently
deduplicating capacity.

## Reservation utility

`ResourceReservationTable` is mutable search state, not mapping output. A
multi-resource reserve is atomic: a failed request leaves every owner
unchanged. `ReservationDelta` plus `undo` provides the lightweight transaction
primitive needed by future backtracking. Owners are explicitly `Node` or
`Edge`; release validates ownership.

## Stage-free mapping

`ModuloMapping` is a completed, deterministic result containing:

* `NodePlacement`: Target node, tile, and modulo issue slot;
* `TransportPlan`: Data/Predicate link steps and explicit `VirtualHold`s;
* `MappedDependence`: required separation and either a transport plan or a
  memory ordering separation.

`LinkStep` records source tile, direction, network domain, and elapsed time
relative to producer issue. `VirtualHold` is the only V0 representation of
waiting/local persistence and has no physical RF index. Same-tile forwarding
is conservatively represented with a hold. Memory edges carry no route.

`requiredSeparationCycles` is the minimum issue-time separation cached for the
future StageScheduler. T-COMP-004 stores it; T-COMP-005 independently checks
transport continuity and recomputes timing. No stage, absolute cycle, route
search state, retry count, or heuristic score is part of the mapping.

## Debug format

`cgra.modulo_mapping.debug.v1` is a deterministic test/reproducer format. It
orders placements by TargetNodeId, dependences by TargetEdgeId, and transport
actions chronologically. It is not a manifest or a target-control encoding.

The next task, T-COMP-005, verifies placement compatibility, resource
conflicts, producer-output timing, link continuity, explicit holds, required
separation, and mapping completeness using this IR.
