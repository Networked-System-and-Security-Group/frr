# R7 extraction manifest (EXT-0 through EXT-3)

This manifest is the source-of-truth for the standalone MIDR extraction.  The
`midrd` process may use common libfrr facilities, but it must build and run
without `bgpd` and without BGP protocol types or behavior.  The existing
`bgpd` implementation remains a differential oracle; it is not a runtime
dependency.

## Runtime ownership

| Area | Standalone owner | Boundary |
|---|---|---|
| identity, canonical admission, ACTIVE/WITHDRAWN, sequence, lifetime | `midr-core.[ch]` | value-only object and identity types |
| wire framing and object codec | `midr-wire.[ch]` | versioned MIDR frame, no BGP UPDATE |
| native IPv4/IPv6 sockets and session callbacks | `midr-transport.[ch]` | MIDR endpoint/frame callback contract over FRR event/stream/buffer |
| source snapshot/events | `midr-prefix-provider.[ch]` | neutral IPv4/IPv6 Prefix events |
| Prefix Feed IPC | `midr-prefix-ipc.[ch]` | local stream, generation and EoR |
| LSDB/TED event publication | `midr-consumer.[ch]` | neutral committed snapshot events |
| orchestration and diagnostics | `midr-context*` / `midrd.c` / `midr-engine.[ch]` | FRR daemon lifecycle and protocol-neutral context |
| optional BGP integration | external adapter | RIB -> Prefix Feed IPC; never included in core |

## Explicitly excluded from `midrd`

`struct bgp`, `struct peer`, `struct bgp_path_info`, BGP attributes,
MP_REACH/MP_UNREACH, AFI/SAFI, TCP/179, update-groups, Adj-RIB-Out, selected
path, BGP Route Refresh, BGP GR/LLGR and the BGP session FSM are not allowed in
the standalone build.  Common libfrr headers and services are allowed.  Zebra
is reached through the public zclient/ZAPI interface; zebra daemon-private
headers and state are not allowed.  A source scan is part of the boundary gate.

## Migration parity gates

The original 18 R5-full-A cases map to the following standalone targets:

* core admission, owned refresh/withdraw, lifetime and floor tests;
* wire and native transport tests for IPv4 and IPv6;
* snapshot/EoR, reconnect and partition recovery tests;
* Prefix Feed snapshot, upsert, withdraw, generation and reconnect tests;
* neutral Consumer snapshot/commit tests;
* three-node IPv4 and IPv6 containerlab smoke without `bgpd`.

## Current migration status

* EXT-0: standalone extraction boundary, neutral wire/transport/Consumer
  contracts and source scan are complete.
* EXT-1: canonical lifecycle, snapshot staging, EOR batch commit, refresh,
  withdraw and expiry are complete in `midr-engine.[ch]` and `midrd`.
* EXT-2: committed Consumer snapshots and the protocol-neutral minimal SPF
  route-result adapter are complete; IPv4/IPv6 parity is covered by unit tests
  and the no-`bgpd` containerlab smoke.
* EXT-3: Prefix Feed IPC is a framed Unix stream with snapshot, upsert,
  withdraw, EoR, disconnect and reconnect handling.  `midrd` accepts
  `--prefix-socket`, supports a pidfile and graceful SIGINT/SIGTERM shutdown,
  and has a top-level Automake install target in addition to the standalone
  developer Makefile.

The old `bgpd` implementation remains a differential oracle.  This manifest
does not authorize its removal before hardening, R5-full-B, R6-B-B and the
parity gate.

## FRR-native migration

The post-extraction migration is tracked in `FRR-MIGRATION-MANIFEST.md`.
During that migration, the allowed dependency boundary changes from libc-only
to common libfrr, while every BGP exclusion above remains mandatory.

F6 (third-group Zebra/FIB data plane) additionally uses libfrr's public
`zclient`/ZAPI surface and the `GRE` ZAPI additions in `lib/zclient.[ch]` plus
`zebra/{zapi_msg.c,zebra_dplane.c,zebra_dplane.h,if_netlink.c,kernel_netlink.c}`.
Those are allowed by the boundary above; zebra daemon-private headers and
in-process zebra state are still forbidden. Because `zclient.h` pulls in
`vrf.h -> vty.h`, `midrd/extraction-boundary-test.sh` compiles the third-group
headers in a second step that keeps `-Wall -Wextra` but cannot keep `-Werror`
(the libfrr headers use the anonymous-struct-member idiom GCC warns about, a
default-on warning with no `-W` option). The include/type scan itself is
unchanged and still rejects `bgpd`/`bgp_`/`zebra/` headers and BGP types.

The old `bgpd` MIDR/Zebra/GRE implementation remains untouched as a differential
oracle: it is not linked into `midrd` and is not removed by F6.
