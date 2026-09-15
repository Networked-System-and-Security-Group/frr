# R7-EXT-0 extraction manifest

This manifest is the source-of-truth for the standalone MIDR extraction.  The
`midrd` core must build without FRR or BGP headers.  The existing `bgpd`
implementation remains a differential oracle until the R5-full-B parity gate.

## Runtime ownership

| Area | Standalone owner | Boundary |
|---|---|---|
| identity, canonical admission, ACTIVE/WITHDRAWN, sequence, lifetime | `midr-core.[ch]` | value-only object and identity types |
| wire framing and object codec | `midr-wire.[ch]` | versioned MIDR frame, no BGP UPDATE |
| native IPv4/IPv6 sockets and session callbacks | `midr-transport.[ch]` | endpoint/frame callback contract |
| source snapshot/events | `midr-prefix-provider.[ch]` | neutral IPv4/IPv6 Prefix events |
| Prefix Feed IPC | `midr-prefix-ipc.[ch]` | local stream, generation and EoR |
| LSDB/TED event publication | `midr-consumer.[ch]` | neutral committed snapshot events |
| orchestration and diagnostics | `midrd.[ch]` / `midr-engine.[ch]` | daemon lifecycle only |
| optional BGP integration | external adapter | RIB -> Prefix Feed IPC; never included in core |

## Explicitly excluded from `midrd`

`struct bgp`, `struct peer`, `struct bgp_path_info`, BGP attributes,
MP_REACH/MP_UNREACH, AFI/SAFI, TCP/179, update-groups, Adj-RIB-Out, selected
path, BGP Route Refresh, BGP GR/LLGR and the BGP session FSM are not allowed in
the standalone build.  A source scan is part of the EXT-0 gate.

## Migration parity gates

The original 18 R5-full-A cases map to the following standalone targets:

* core admission, owned refresh/withdraw, lifetime and floor tests;
* wire and native transport tests for IPv4 and IPv6;
* snapshot/EoR, reconnect and partition recovery tests;
* Prefix Feed snapshot, upsert, withdraw, generation and reconnect tests;
* neutral Consumer snapshot/commit tests;
* three-node IPv4 and IPv6 containerlab smoke without `bgpd`.

EXT-0 only establishes the build and test boundaries.  It does not claim that
the complete parity behavior has already migrated.
