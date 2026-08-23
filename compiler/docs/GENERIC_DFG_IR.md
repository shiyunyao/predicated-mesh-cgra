# Generic Loop DFG IR

The Generic DFG is the target-independent semantic input to later compiler
passes. Version 1 represents one innermost natural loop: one logical loop
iteration is stored once, and loop-carried dependences use a non-negative
iteration distance instead of duplicating iterations.

## Boundary

The IR contains executable program operations, values, constants, predicates,
memory ordering, and dependence distances. It does not contain LLVM classes,
PHI nodes, basic blocks, PE coordinates, modulo slots, routes, registers,
scratchpad banks/ports, or target numeric encodings. A later target
legalization pass maps generic operations to a target's supported operations.

`Opcode::ICmp` carries a generic `ICmpPredicate`; signed comparisons and
`AShr` are representable even when the current CGRA target cannot execute them.
There is deliberately no `Phi` opcode. Frontend lowering must turn control
merges into `Select` and loop-carried PHI state into explicit dependence edges.

## Types and Operations

`ValueType` supports integer, predicate, float, and void values. Canonical
helpers include `i1`, `i8`, `i16`, `i32`, `f32`, `predicate`, and `voidTy`.
`Load` and `Store` carry `MemoryOpInfo` (`accessWidthBits` and `isVolatile`). A
store's result type is `void`; the stored value is represented by an operand,
not by a fake result edge.

Initial generic operations are:

```text
Add Sub Mul And Or Xor Shl LShr AShr ICmp Select Load Store
```

## Operands and Edges

Logical operand indices are shared by data and predicate edges:

```text
binary / shift / ICmp: operand 0 = lhs/value, operand 1 = rhs/shift amount
Select:                operand 0 = predicate, 1 = true value, 2 = false value
Load:                  operand 0 = address
Store:                 operand 0 = address, 1 = store data, 2 = commit predicate
```

Node-produced operands are supplied by incoming value edges. Live-ins and
constants are first-class non-executable values and are attached using
`ExternalValueRef` or `ConstantRef` bindings. A `LiveOut` names a node result
that leaves the loop without introducing a fake executable node. Every edge
has a stable ID, source, destination, and non-negative `distance`:

```text
producer in iteration i -> consumer in iteration i + distance
```

`DataEdgeInfo` and `PredicateEdgeInfo` carry the destination operand index.
`MemoryEdgeInfo` carries one of `RAW`, `WAR`, or `WAW` and carries no value;
memory edges never allocate a physical route.

The container accepts directed cycles. For example, a recurrence can be
represented with an edge `%n1 -> %n1` and `distance = 1` without copying the
loop body.

## IDs, Ownership, and Determinism

`DFG` owns nodes, edges, constants, external values, live-outs, and bindings by
value. Public handles are `NodeId`, `EdgeId`, `ExternalValueId`, `ConstantId`,
and `LiveOutId`.
Readers expose const spans; mutation is performed through `DFGBuilder`.
The V1 builder allocates each handle monotonically for deterministic fixtures;
callers must use ID lookup and must not treat an ID as a storage-vector index.
Insertion IDs are stable for the graph lifetime and all public iteration is in
ID order. Adjacency queries are maintained directly rather than scanning all
edges.

The builder rejects duplicate operand providers, unknown references, invalid
operand indices, and invalid endpoints. Full semantic legality, such as type
compatibility and operation-specific operand rules, belongs to the independent
DFG verifier in T-COMP-002.

## Debug JSON

`cgra::ir::writeJson` and `readJson` implement the versioned debug schema
`cgra.dfg.debug.v1`. JSON arrays are ordered by stable ID and bindings are
serialized by `(node, operand)`, so serialization is deterministic and suitable
for fixtures and failure artifacts. The `live_outs` array records each output's
ID, name, type, and source node. This is a debug/test interchange format, not a
permanent compiler ABI.

Canonical reusable fixtures are provided in `compiler/tests/IR/Fixtures.*`:

```text
simple_add, arithmetic_chain, fanout, recurrence,
load_add_store, predicate_select, predicated_store, memory_dependence
```

Each fixture is round-tripped through JSON by the IR test suite.
