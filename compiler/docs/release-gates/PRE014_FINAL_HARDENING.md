# PRE014 Final Hardening

This document records the final backend hardening evidence before T-COMP-014.
It is intentionally not a GO decision until the hosted feature-head, PR
merge-ref, and post-merge `main` checks have all completed successfully.

## Candidate

- Branch: `compiler/target-contract-v2`
- Baseline: `6ba4a0bef861abb49733451d72f4e8546ce750b9`
- Final candidate SHA: `e8320f35b5e2127bee2cf2aae3fc8376ef38b1c9`
- Draft PR: [#1](https://github.com/shiyunyao/predicated-mesh-cgra/pull/1)
- Merged main SHA: pending

## Local Hardening

The tiny exact oracle now independently enumerates finite modulo placement and
routing candidates for Data, Predicate, and Memory edges. It is bounded to six
tiles, five nodes/edges, and II three, and calls T005 only for final candidate
legality. Routed tests cover Data, Predicate, recurrence, mixed Memory/Data,
determinism, and a forced disconnected no-route case. The routed comparison
corpus uses fixed seed `0xC1A0100`.

RF regressions cover:

- SELECT with two DataRF operands and physical read ports `{0, 1}`;
- FU-result W0 plus network W1;
- two W1-only writes rejected despite aggregate capacity;
- predicate FU/network W0/W1 asymmetry and verifier corruption;
- LSU load-result W1 compatibility;
- corrupted physical-port assignments rejected independently.

The canonical resource model still uses direct semantic-key lookup. A
disconnected test target is supported explicitly so an absent link is a normal
no-route result rather than a resource-ID exception.

## Local Gates

| Gate | Result |
| --- | --- |
| dev-debug build | PASS |
| dev-debug CTest | PASS (17/17) |
| ci-debug CTest | PASS (20/20) |
| sanitizer CTest | PASS (19/19 executed; CTest IDs through 20) |
| `make compiler-e2e` | PASS (real golden/RTL replay and observations) |
| `make regression` | PASS |

The local runs were performed from the repaired worktree after the routed
oracle and RF regression changes. The final candidate SHA will be filled in
after the release commit is created.

## Hosted Evidence

The compiler-fast and hardware-regression workflows include push coverage for
`compiler/target-contract-v2`. Record exact URLs and conclusions here only
after GitHub has run the pushed final SHA.

- Exact feature-head `compiler-fast-gate`: PASS ([run 32753316169](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32753316169))
- Exact feature-head `hardware-regression-gate`: PASS ([run 32753316101](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32753316101))
- Final feature-head `compiler-fast-gate`: PASS ([run 32754228557](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32754228557))
- Final feature-head `hardware-regression-gate`: PASS ([run 32754228462](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32754228462))
- Draft PR merge-ref `compiler-fast-gate`: PASS ([run 32754361570](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32754361570))
- Draft PR merge-ref `hardware-regression-gate`: PASS ([run 32754361625](https://github.com/shiyunyao/predicated-mesh-cgra/actions/runs/32754361625))
- Post-merge `main` `compiler-fast-gate`: pending
- Post-merge `main` `hardware-regression-gate`: pending

## Re-audit

Required source-audit patterns were checked for fixture-specific primary-E2E
II/VirtualHold overrides, operand-index RF ports, FU/non-FU write-port
guessing, semantic ConstantId physical addresses, mapper/route-search reuse by
the exact oracle, and linear full-resource hot lookups. No unreviewed
production shortcut was found. Final source audit: GO for the feature-head
candidate, including both feature-head and PR merge-ref checks. Post-merge
`main` evidence remains pending until the PR is merged.

## Non-blocking Limitations

The backend still intentionally has no Kernel ABI, LLVM frontend, runtime
symbolic trip count, MVE/rotating registers, spills, multicast routing, or
CGRA-Bench coverage. These remain outside PRE014 and block no release decision
once the hosted evidence is green.
