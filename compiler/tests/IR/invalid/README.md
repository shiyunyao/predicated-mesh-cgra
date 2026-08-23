# Invalid DFG Corpus

This directory is the named negative-case inventory for T-COMP-002. The
executable `DFGVerifierTest.cpp` constructs the semantic cases directly with
`DFGBuilder` so production construction APIs remain read-only and do not gain
unchecked mutation hooks. Cases that cannot become a `DFG` because the JSON
reader rejects unknown references are tested at that parser boundary.

| Case | Boundary exercised |
| --- | --- |
| `missing_operand_provider` | verifier: provider completeness |
| `duplicate_operand_provider` | verifier: provider uniqueness (cross-kind) |
| `operand_index_out_of_range` | verifier: edge operand bounds |
| `data_type_mismatch` | verifier: exact provider type |
| `predicate_type_mismatch` | verifier: predicate operand domain |
| `void_value_used` | verifier: void result use |
| `icmp_missing_predicate` | verifier: ICmp metadata |
| `icmp_wrong_result` | verifier: predicate result |
| `icmp_mismatched_operands` | verifier: compare operand types |
| `select_nonpredicate_condition` | verifier: Select condition |
| `select_mismatched_values` | verifier: Select value types |
| `select_wrong_result` | verifier: Select result type |
| `load_bad_address` | verifier: Load address |
| `load_void_result` | verifier: Load result |
| `store_nonvoid_result` | verifier: Store result |
| `store_bad_address` | verifier: Store address |
| `store_bad_predicate_if_applicable` | verifier: Store commit predicate |
| `data_edge_from_predicate` | verifier: data edge source domain |
| `predicate_edge_from_data` | verifier: predicate edge source domain |
| `memory_edge_from_nonmemory` | verifier: memory source endpoint |
| `memory_raw_wrong_endpoints` | verifier: RAW endpoint pair |
| `memory_war_wrong_endpoints` | verifier: WAR endpoint pair |
| `memory_waw_wrong_endpoints` | verifier: WAW endpoint pair |
| `unknown_edge_source` | JSON parser: unknown source reference |
| `unknown_edge_destination` | JSON parser: unknown destination reference |
| `unknown_external_binding` | JSON parser: unknown external reference |
| `unknown_constant_binding` | JSON parser: unknown constant reference |

The first twenty-three cases are represented by focused builders in the test
binary; the last four are input-boundary cases because `DFGSerialization` must
reject them before constructing a graph. This index is kept stable so later
fuzzing and reproducer artifacts can use the same names.
