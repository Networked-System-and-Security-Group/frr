Feature: MIDR local origination, RIB selection, LSDB and production TED

  @M4-RIB-001
  Scenario: Direct owner and sequence fallback select one authoritative path
    Given direct-owner and indirect candidates for one MIDR identity
    When candidates are replaced or withdrawn
    Then the MIDR RIB selects by owner, sequence and stable tie-break rules

  @M4-RIB-002
  Scenario: Conflicting highest-priority payloads are quarantined
    Given equal-priority paths with the same identity and sequence
    When their immutable payloads differ
    Then the identity remains in the RIB without a selected path

  @M4-RIB-003
  Scenario: Owned objects suppress measurement noise and fail closed
    Given Local Facts for one Membership and Link
    When versions, measurements, policy or persistent sequence state change
    Then only protocol-visible changes are originated and persistence failures withdraw authority

  @M4-LSDB-001
  Scenario: Missing Membership keeps dependent objects pending
    Given selected Link and Prefix objects with a missing endpoint Membership
    When the Membership appears or disappears
    Then LSDB dependencies and the production TED are rederived atomically

  @M4-LSDB-002
  Scenario: Failed TED preparation preserves the committed transaction
    Given a READY LSDB and TED generation
    When construction of the next TED snapshot fails
    Then the old LSDB, TED and held snapshot remain readable until retry succeeds

  @M4-LSDB-003
  Scenario: Real LSDB and Mock Provider produce the same TED
    Given equivalent Membership, Link and Prefix inputs
    When one snapshot is derived from selected RIB paths and one from the test-only Mock Provider
    Then their normalized router-level and group-level TED views are identical
