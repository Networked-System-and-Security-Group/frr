# midrd FRR-native migration manifest

This manifest records the executable boundary for plan 17.  It is an
implementation checklist, not a replacement for the protocol design.

## Baseline

```text
branch: feat/yhy-midr-single-instance-ls-flooding
protocol code baseline: 5eab85832a
documentation baseline: d0ccfa470a
component gate: 20 programs plus extraction-boundary-test.sh
```

The `bgpd` implementation remains unchanged as a differential oracle.  It is
not linked into `midrd` and is not copied into the new runtime.

## Dependency boundary

Allowed common FRR facilities:

```text
libfrr daemon lifecycle and event master
event, stream and buffer
memory types and zlog
prefix, ipaddr, sockunion and ifindex_t
zclient and public ZAPI structures
```

Forbidden dependencies:

```text
bgpd headers and objects
struct bgp, struct peer and struct bgp_path_info
BGP OPEN/UPDATE/FSM/Capability and AFI/SAFI state
selected path, alternate path, Adj-RIB-Out and update-group
zebra daemon-private headers or in-process zebra state
```

FRR I/O types are backend details.  They must not become part of the topology
input, canonical object, committed TED/SPF or route-result contracts.

## Runtime ownership migration

| Current owner | Current mechanism | Target phase | Target mechanism |
| --- | --- | --- | --- |
| `midrd.c` | private `struct midrd` | F1 | opaque public `struct midr_context` with private layout |
| `midrd.c` | `signal()`, manual pidfile and polling loop | F2 | FRR daemon lifecycle, signal and event master |
| `midr-transport.c` | socket + `select()` + private byte buffers | F3 | FRR event + stream/buffer, preserving MIDR session generation |
| `midr-prefix-ipc.c` | Unix socket + `select()` | F4 | FRR event + stream/buffer, unchanged Prefix wire contract |
| `midr-local-ipc.c` | Unix socket + `select()` | F4 | optional FRR-backed external/test Provider |
| first-group adapter | external Local Fact IPC | F5 | direct `midr_topology_*` calls inside `midrd` |
| `bgp_midr_zebra.c` | state attached to `struct bgp` | F6 | zclient and route state attached to `struct midr_context` |

## Semantic invariants

The migration must preserve:

```text
one canonical instance per identity
monotonic sequence admission and conflict isolation
ACTIVE/WITHDRAWN, lifetime and floor behavior
snapshot/EoR staging and atomic commit
old consistent TED view during failed or incomplete input
session generation and stale-completion rejection
frame-written versus frame-queued distinction
IPv4 and IPv6 behavior
```

## Gate cadence

Small commits run the affected module tests and `git diff --check`.  Each of
F1 through F4 ends with all 20 component programs and the boundary scan.  Full
FRR rebuilds, containerlab, sanitizer, scale and long-soak gates run at the
phase exits defined by plan 17 rather than after every local edit.
