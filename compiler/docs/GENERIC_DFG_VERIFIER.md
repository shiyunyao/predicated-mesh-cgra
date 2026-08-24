# Generic DFG Verifier

`cgra::ir::DFGVerifier` is the semantic oracle for the target-independent loop
DFG. It accepts only a `const DFG&`, never modifies or repairs the graph, and
does not inspect `TargetModel`, LLVM, RTL, mapping, or scheduling state.

## Boundary

The verifier answers whether a graph is a valid representation of one generic
loop iteration with explicit loop-carried dependences. It does not answer
whether the current CGRA can execute the graph. Target operation support,
scratchpad capacity, PE resources, RF ports, routes, II, and RecMII belong to
later target legalization and mapping passes.

Consequently, generic `AShr` and signed `ICmp` such as `SLT` are valid even if a
particular target does not implement them. Directed cycles are valid: an edge
means `src(iteration i) -> dst(iteration i + distance)`, and `distance` is
non-negative by its unsigned IR type. The verifier does not prove that a
frontend discovered every required memory dependence.

## Provider rules

Every logical operand index must have exactly one provider:

- an incoming `Data` edge;
- an incoming `Predicate` edge;
- an external-value binding; or
- a constant binding.

Memory edges are ordering constraints only. They never provide an operand and
never allocate a physical route. Data and predicate edges use the global
operand numbering defined by [GENERIC_DFG_IR.md](GENERIC_DFG_IR.md).

The verifier checks exact type equality. It does not insert or infer casts.
Void results cannot be used as values. A `Store` is void and may not produce a
data or predicate value.

## Opcode semantics

Arithmetic and bitwise operations require two equal non-void data operands and
an equal result. Shifts require an integer value, integer shift amount, and a
result matching the value. `ICmp` requires two equal-width integers, an
`ICmpPredicate`, and a predicate result. `Select` requires a predicate at
operand 0 and equal non-void values at operands 1 and 2. `Load` has one integer
address operand and a non-void result. `Store` has integer address, non-void
data, and an optional predicate commit operand, with a void result.

Load/Store `MemoryOpInfo` is checked for presence and a non-zero byte-aligned
access width. Physical address ranges and target memory widths are outside the
generic verifier boundary.

Memory edges are restricted to canonical endpoint pairs:

```text
RAW: Store -> Load
WAR: Load  -> Store
WAW: Store -> Store
```

This proves that present memory edges are well-formed; it does not prove
memory-dependence completeness.

## Diagnostics

Diagnostics use stable `DFG_*` codes and carry node, edge, operand, external,
constant, or live-out context where relevant. The report is valid exactly when
it has no errors. Independent errors are accumulated in deterministic graph
iteration order. `format()` is intended for humans and `toJson()` uses the
versioned `cgra.dfg.verification.v1` schema for CI and reproducer artifacts.

## CLI

After configuring the compiler:

```bash
cmake --build --preset ci-debug
compiler/build/ci-debug/bin/cgra-dfg-verify input.dfg.json
compiler/build/ci-debug/bin/cgra-dfg-verify input.dfg.json \
  --json-report build/verification.json
```

Exit status is `0` for a valid graph, `1` for a verification failure, and `2`
for an input/parse/CLI error.

## Pipeline use

Every graph-producing or graph-transforming pass should verify before handing a
graph to the next pass:

```text
producer/parser/pass -> Generic DFG -> DFGVerifier -> next compiler pass
                                      \-> hard failure with report
```

The verifier is intentionally separate from `DFGBuilder`; builder success is
not semantic proof. Full ABI placement, dominance/SSA reconstruction, alias
analysis, optimization, target legalization, and mapping are deferred.
