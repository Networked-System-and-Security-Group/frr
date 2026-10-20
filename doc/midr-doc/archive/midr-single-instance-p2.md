# MIDR single-instance flooding: P2

## Scope

P2 switches the MIDR production path from peer-candidate flooding to one
canonical instance per LS Object identity. The identity is the complete NLRI
key, including object type, originator and any IPv4/IPv6 prefix fields.

The production path is:

```text
UPDATE decode
  -> instance validation
  -> canonical RIB admission
  -> RIB event
  -> LSDB/TED derivation
  -> flood the canonical instance
```

Propagation Path is not part of the P2 data model, attributes, packet codec or
selection logic.

## Wire format

MIDR continues to use the single outer family `AFI_BGP_LS / SAFI_MIDR_LS`.
IPv4 and IPv6 are encoded by the object key and address TLVs; no IPv6 MIDR
session family is introduced.

Each P2 UPDATE carries one semantic NLRI. Its MIDR attribute contains the
sequence, state and age TLVs together with the object-specific active payload.
An ACTIVE instance carries the complete payload. A WITHDRAWN instance carries
the complete NLRI key, sequence, state and age, with no active payload.

The decoder rejects truncated, duplicated, out-of-order or unknown TLVs,
invalid state, zero sequence, invalid keys, non-canonical prefixes and
address-family mismatches before changing RIB state. Age is transport lifetime
information; it is not part of instance equality or attribute interning.

## Canonical RIB

For every identity the RIB keeps one current canonical instance. A higher
sequence replaces the current instance, a lower sequence is ignored, and an
equal semantic instance is a duplicate that does not restart its lifetime or
create another event. Equal sequence with different content quarantines the
identity until a newer valid instance arrives.

Peer state is retained only as an advertisement relationship. `MP_UNREACH`
removes the relationship from the sending peer; it does not remove the
canonical owner instance. An owner's withdrawal is a higher-sequence
WITHDRAWN instance and must pass through canonical admission like any other
instance.

The RIB exposes two views:

- The active view returns ACTIVE instances only.
- The flooding view also retains the selected WITHDRAWN instance so the
  correction can be propagated.

Canonical events retain immutable before/after records until downstream work
has been prepared and the event is acknowledged. Allocation or queue failure
does not replace the current instance.

## Receive and send paths

The receive path decodes the complete NLRI and attribute before calling the
canonical RIB. A self-originated sequence is passed to the owner sequence
observer for fightback; it is not silently discarded.

The send path serializes the selected immutable MIDR path attribute. The
object payload and sequence therefore cannot change because a later RIB event
replaces the canonical instance. The transmitted age is derived from the
canonical record at serialization time. Encoding failure rolls the stream back
and prevents a partial UPDATE.

The packet path is deliberately limited to one NLRI per UPDATE in P2. This
keeps the age and instance snapshot unambiguous; batching is outside this
phase.

## LSDB and TED

Only ACTIVE canonical instances enter the usable LSDB and TED input. A
WITHDRAWN instance remains available to the flooding view but removes the
corresponding active LSDB/TED identity. Existing copy-on-write snapshots,
generation numbers, callback replay and usable gates remain in force.

The LSDB export gate also checks the peer advertisement relationship, so a
canonical instance is not sent back to a peer that already advertised that
identity. Global and same-group export scope is still decided by the existing
membership view; no propagation-path length or owner preference is used.

## Ownership

Locally originated objects enter the same canonical RIB path as received
objects. Local removal creates a higher-sequence WITHDRAWN instance. Sequence
allocator failure does not fabricate a withdrawal: the existing object is
preserved, the owner is marked not ready and retry remains pending.

## Verification

The focused P2 component programs cover IPv4/IPv6 object keys, ACTIVE and
WITHDRAWN codec paths, malformed input, duplicate and conflict admission,
peer `MP_UNREACH`, owner withdrawal, RIB iteration, Owned sequence handling,
LSDB replay and TED lifecycle. The verified targets are:

```text
test_midr_attr
test_midr_codec
test_midr_instance
test_midr_ls_object
test_midr_packet
test_midr_prefix
test_midr_rib
test_midr_owned
test_midr_scope
test_midr_lsdb
test_midr_ted
```

The current P2 worktree was verified with the following additional checks:

```text
make -j2 bgpd/bgpd                              PASS
ASAN_OPTIONS=detect_leaks=0 make -B vtysh/vtysh PASS
midr-test/run-ted-fixtures.sh all               12/12 PASS
```

The component and compatibility regression binaries passed for attributes,
codec, instance, LS Object, packet, prefix, RIB, Owned, scope, LSDB, TED,
input, resynchronization, sequence, cost, SAFI, synchronization and the SPF
compatibility paths. The build also reports existing warnings in unchanged
MIDR auxiliary modules and an existing Python/LeakSanitizer report from FRR
clippy when the leak check is not disabled. The latter does not reproduce
when the generator is run with leak detection disabled; no warning was
reported from the P2-modified files. No Propagation Path reference remains in
production or test code.

P2 does not implement the later refresh/aging policy, floor garbage
collection, complete resynchronization protocol, non-graceful-loss READY
policy or third-group FIB integration.
