# MIDR single-instance flooding: R3 reclaim and scale evidence

## Scope

R3 closes the identity and canonical-floor lifetime boundary and adds a
repeatable capacity/scale gate.  The implementation is in commit
`d3730eb0bf` on `feat/yhy-midr-single-instance-ls-flooding`.

The production floor-GC switch remains disabled by default.  R2 established
`H = L` only under the stated per-hop age-budget assumptions; R3 does not
turn that conditional proof into a deployment-level guarantee.

## Ownership and lifetime rules

The relevant references are kept separate:

```text
canonical current instance
        |
        +-- pending canonical event (before/after)
        +-- retired floor hold after expiry
        +-- packet/template references
        |
        +-- RIB identity
              +-- route-node creation hold
              +-- current canonical path
              +-- retired path hold after path reap
              +-- per-peer advertisement records
                    |
                    +-- LSDB committed/staging path and destination locks
```

When an ACTIVE canonical instance reaches its lifetime, expiry transfers the
current reference to the expiry event and creates a separate retired hold.
The RIB removes the canonical path from the active view but retains a path
lock until update-group and LSDB users release it.  A floor is reclaimable
only after its retention interval, all canonical events for the key, and all
retired instance references have drained.

Canonical GC accepts a boolean reclaim callback.  The RIB callback vetoes
reclamation while an identity has paths, a retired path with external locks,
or an LSDB obligation.  The LSDB callback vetoes while the committed LSDB or
dirty staging list still contains the identity.  Once the callback permits
the operation, the RIB detaches the identity, releases its route-node hold,
returns the synthetic ID to the allocator, and canonical GC releases the
floor hold.  A newer version supersedes a floor and releases only the floor's
private retired hold; packet/template references remain independently valid.

The route-node hold is intentional: removing the last path must not destroy
the destination before the canonical floor retention window has completed.
TED snapshots contain immutable value arrays and an independent snapshot
reference; they do not retain RIB identities or paths and therefore do not
block identity reclamation.

## Error and diagnostic behavior

`-ENOSPC` and `-ENOMEM` remain resource outcomes handled by the R1 session
retry/resynchronization path.  `-ERANGE` is not a resource outcome: it denotes
a monotonic-time or internal age invariant failure, is counted separately as
an internal rejection, and does not enter the resource backoff path.

`show midr rib` and `midr_rib_summary` expose canonical floors, pending
canonical events, retired-reference count and retired-reference bytes in
addition to identity, path and advertisement counts.

## Scale gate

`midr-test/run-midr-scale-gate.sh` builds the current worktree in an isolated
container tree and runs `test_midr_scale` over an objects-by-peers matrix.
Each point emits fill, midlife and reclaimed rows and checks that all identity,
canonical, path and advertisement state is reclaimed after the lifetime and
LSDB staging passes.  The default matrix runs two churn cycles at:

```text
100 x 2    100 x 8    1000 x 2    1000 x 8    5000 x 4
```

The target-limit run adds one cycle at `65536 x 1`.  Its fill row reached
`identities=65536`, `canonical=65536`, `advertisements=65536` and
`paths=65536`; its reclaimed row returned all these counters to zero.

Evidence for the final run is stored at:

```text
/home/guest/yhy/midr-gate-runs/r3-final-20260914T075828Z-scale
```

The runner validates matrix syntax, rejects zero or malformed points, checks
the expected number of phase rows, and uses a root container user so the
isolated `/build` tree is writable.  The component manifest includes
`test_midr_scale`.

## Verification

The R0 worktree component gate passed 22 MIDR component programs and all 12
TED fixture scenarios:

```text
COMPONENT_SUMMARY programs_passed=22 programs_failed=0 fixtures=passed
COMPONENT_RESULT=0
GATE_RESULT=0
```

Evidence:

```text
/home/guest/yhy/midr-gate-runs/r3-final-component-20260914T081729Z-worktree
```

The focused GCC tests passed for `test_midr_instance`, `test_midr_rib`,
`test_midr_packet` and `test_midr_scale`.  The corresponding ASAN/UBSAN/LSAN
run passed with leak detection enabled and no sanitizer finding.  The
large-scale run also exposed and fixed the signed left shift in
`lib/id_alloc.c` for offset 31.

Source-tree artifact scanning and `git diff --check` are clean.  Existing
warnings in unchanged `bgp_midr_pm.c` and `midr_trace_scheduler.c` remain
outside the R3 changes.

## Boundaries

R3 proves the in-process reference and slot-reuse rules under virtual-time
tests and the stated synthetic scale points.  It does not prove that a
production network and remote kernel keep every unmeasured interval below the
R2 budget `B`; production floor GC therefore remains off.  Real multi-node
withdrawal, restart/reconnect, normal withdrawal and short-`L` long-stability
scenarios remain R5 work, and independent `midrd` extraction remains P7.
