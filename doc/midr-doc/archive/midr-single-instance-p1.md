# MIDR single-instance flooding: P1 core

## Status

P1 adds the instance codec and canonical database primitives. Production packet,
RIB, owned-object and LSDB paths are not switched in this commit. P2 must wire the
new core and remove Propagation Path together; there is no runtime mode switch or
automatic old/new wire negotiation.

The public provider and TED interfaces are unchanged. The existing object payload
and identity helpers are reused. The new internal `midr_instance` wraps the object
with ACTIVE or WITHDRAWN state; age is not semantic content.

## Wire

The new attribute entrypoints require SEQUENCE (TLV 1, uint64), STATE (TLV 4,
uint8: ACTIVE=1, WITHDRAWN=2) and AGE (TLV 5, uint32 milliseconds). Integers use
network byte order. Known TLVs remain strictly ordered and unique; unknown TLVs
are rejected, matching the existing strict decoder policy.

ACTIVE retains the object-specific required fields and optional policy tags.
WITHDRAWN carries only sequence, state and age; its original NLRI key remains
complete. Construct local withdrawals with a zeroed payload and policy tags.
Zero sequence and invalid state are rejected. The new decoder does not accept
legacy attributes with missing state/age; the unchanged legacy caller does not
accept the new fields.

Attribute decode and key-specific validation are separate operations so P2 can
validate all keys in an UPDATE before applying them. Age at or above the configured
maximum is structurally representable, but canonical admission returns EXPIRED.

## Canonical database contract

- Calls are serialized by the owner event loop; callers supply a monotonic clock.
- Each key has one current instance or a quarantined/floor entry. There are no
  peer candidates, alternate selection or owner preference.
- Higher versions replace; equal content is a duplicate with no allocation,
  pending work or lifetime extension; lower versions are reported without mutation.
- Equal versions with different content quarantine the key. Further equal inputs
  neither reselect a payload nor restart its lifetime. A newer valid version can
  recover the identity.
- A new identity, immutable record and bounded pending event are prepared before
  commit. Allocation/capacity errors leave the previous version and event queue
  unchanged. Existing keys can update at the identity limit, subject to actual
  event/memory capacity.
- An event retains the prior and new immutable record references. Peek it, secure
  downstream propagation/dirty work, then acknowledge it. Do not acknowledge on
  failed downstream preparation. P2 can retain an old WITHDRAWN for old-scope
  delivery when a newer ACTIVE has replaced it; retained history is never a
  candidate for canonical selection.
- Acquired references survive event acknowledgement and database destruction.
  The allocator context must outlive all held references. All reference operations
  are single-threaded, like the database.
- Lookup hides an expired current record even if a pending expiry event cannot be
  allocated. Explicit expiry queues the local removal and retains a floor. P1
  implements no floor garbage collection, owner refresh timer or remote expiry
  notification.

The default recoverable allocation path uses calloc/free; a zeroing allocator and
matching free callback can be supplied for accounting and deterministic failure
injection. Identity/event counts are available. Full queue/snapshot byte accounting
and public NOT_READY propagation belong to P2/P5, not to this component alone.

## Verification

Build and execute `tests/bgpd/test_midr_instance` using the generated libtool
wrapper, not the raw `.libs` binary without its library path. The test covers:

- All four types and both prefix families; active and unknown withdrawn instances.
- Golden withdrawal bytes, IPv6 /0, /64, /65, /128, malformed/truncated/duplicate
  TLVs, invalid combinations and no-space rollback.
- Higher/equal/lower comparison, conflict expiry and recovery, 4096 duplicate
  inputs without additional allocations, and no lifetime extension.
- Conservative age rounding, budget saturation, huge elapsed time and clock rollback.
- Failure of each initial/update allocation, event/identity capacity limits,
  expiry failure, retained old withdrawals and held references after destruction.

On NetArchLab90, GCC built bgpd, vtysh and the test targets. The new test and all
19 P0 component programs passed; all 12 TED fixture scenarios passed. The changed
core, codec, identity helpers and test were also compiled and run with ASAN/UBSAN
under GCC 11.4 and Clang 14, with leak detection enabled. The linked libfrr was the
ordinary P0 library, not a fully sanitizer-instrumented FRR build.

Evidence is under
`/home/guest/yhy/midr-dev-runs/single-instance-ls/p1/`.
An initial test helper naming conflict with the socket accept() declaration was
fixed before final regression. Final core builds contain no new warnings.

P1 does not verify real MP_REACH/MP_UNREACH transport under the new format, live
flooding convergence, generation changes, automatic floor reclamation, or the
end-to-end delay budget. Those remain P2-P6 acceptance work.
