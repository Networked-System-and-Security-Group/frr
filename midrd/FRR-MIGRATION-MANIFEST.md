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
| `bgp_midr_zebra.c` | state attached to `struct bgp` | F6 (done) | `midrd/midr-dp-backend.[ch]` implements `struct midr_zebra_backend_ops` over a midrd-owned zclient; `midrd/midr-gre.[ch]` carries GRE provisioning on the same client |

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

## F6 completion notes (third-group data plane)

F6 is implemented and verified; the old `bgpd` implementation is untouched and
still serves as the differential oracle.

| Area | File | Notes |
| --- | --- | --- |
| operations table | `midrd/midr-dp-backend.[ch]` | route_add/route_del/update_deferred/flush/abort_pending |
| zclient | `midrd/midr-dp-backend.c` | `zclient_new()` + `zclient_init(zc, ZEBRA_ROUTE_BGP_MIDR, 0, &privs)`; path from `--zserv-path` (`frr_zclientpath`) |
| encoding | `midrd/midr-dp-backend.c` | BASIC/ECMP/UCMP/SRv6, instance 0/1, distance 115, `ZEBRA_FLAG_ALLOW_RECURSION`, `ZEBRA_FLAG_USE_RECURSIVE_WEIGHT` |
| installed state | `midrd/midr-dp-backend.c` | the hash is updated only after zebra accepted the message; a failed send never fakes installed state |
| deferred batch | `midrd/midr-dp-backend.c` | 100 ms one-shot timer; a deferred failure keeps the un-applied batch and the recovery timer retries it before calling `midr_spf_install_resync()` |
| zebra reconnect | `midrd/midr-dp-backend.c` | clear the installed hash and `midr_spf_install_replay()` |
| GRE | `midrd/midr-gre.[ch]` | `midr_gre_init(master)` after the backend, shares its zclient; `lib/zclient.[ch]` + `zebra/` carry the ZAPI/netlink side |
| lifecycle | `midrd/midrd.c` | `--zserv-path`, `--vrf-id`, `--no-zebra`; backend start after context init; shutdown order GRE fini -> backend stop (unregister/withdraw/flush) -> zclient destroy |

Verification evidence (container `frr-ubuntu24-ymy`, log paths on the build
host): FRR top-level `make -j112` EXIT=0 with 0 warnings (`/tmp/build-full.log`);
midrd component suite 25 programs PASS including `midrd-dp-test` and the
boundary scan (`/tmp/verify-comp.log`); GRE two-container connectivity
PASS=24/FAIL=0 (`/tmp/gre-run/test2.log`); group-3 isolated ZAPI/zebra/Linux-FIB
test PASS=29/FAIL=0 (`/tmp/fib-final.log`).

Known open item: zebra kept the SPF instance selected in the kernel FIB after
the TE instance was added (dual-instance coexistence is verified at the ZAPI
level and by `midrd/dp-backend-test.c`, but the FIB promotion needs a zebra-side
follow-up).


Small commits run the affected module tests and `git diff --check`.  Each of
F1 through F4 ends with all 20 component programs and the boundary scan.  Full
FRR rebuilds, containerlab, sanitizer, scale and long-soak gates run at the
phase exits defined by plan 17 rather than after every local edit.
